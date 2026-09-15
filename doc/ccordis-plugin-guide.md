# cordis 插件实现指南

> 迁移说明 (2026-09-15): 本文档随 ccordis 独立化从 `SpectrumExplorerDemo/doc/` 迁入本项目 `doc/`。文中旧路径对照: 内核代码 `ccordis/` → `include/ccordis/`(公共头) + `src/`(实现); `plugins/hello` → `examples/hello`; 生命周期细节见 `plugin-lifecycle.md`。

> 状态: 指南 v1 (2026-09-14)
> 范围: 面向插件作者的实操手册——如何用三种插件形态实现 GUI 视图、数据处理、应用层协议插件，以及动态插件打包与测试
> 读者: 在本插件系统上开发功能模块的 C++ 工程师
> 前置阅读: `ccordis-plugin-system-design.md`（设计契约）、`ccordis-selection-report.md`（选型）、`plugin-lifecycle.md`（生命周期逐条细化）
> 内核状态: v2 纯 C++17 已实现并全绿（8 TU 纯净性 + 12 场景冒烟 + ASAN/UBSAN）

---

## 0. 五分钟上手

最小插件（函数形态）——注入服务、订阅事件、提供一个新服务：

```cpp
#include "ccordis/Context.h"
using namespace cordis;

Context ctx;   // 宿主通常在 main() 创建一次

ctx.plugin("demo.hello",                                    // 唯一 id
           PluginMeta{"demo.hello", "演示", {}, {"demo.greet"}},
           [](Context &c, const Value &cfg) {               // 插件体
               auto model = c.inject<SignalModel>("model.core");
               auto rate = cfg.at("rateHz").toInt(5);       // 配置缺省值就地给
               c.provide<Greeter>("demo.greet", std::make_shared<Greeter>(rate));
               c.on("app.ready", [](const Value &) { /* ... */ });
           },
           Value::object({{"rateHz", 5}}));                 // 配置
```

### 三形态选择

| 形态 | 写法 | 适用 | 不适用 |
| --- | --- | --- | --- |
| **函数** `plugin(name, meta, applyFn, cfg)` | 一个 lambda | 无状态或状态可全捕获的轻模块 | 需要析构顺序保证的重资源 |
| **类** `plugin<T>(name, cfg, required)` | `T(Context&, const Value&)` 构造函数即插件体，析构在拆撤时触发 | GUI 视图、协议连接等有明确生命周期的对象 | — |
| **对象** `plugin(name, iplugin*, cfg, required)` | 预构造 `IPlugin` 实例，`apply()` 激活 | 实例由外部框架创建/复用 | 所有权不想留在调用方时 |
| **动态 .so** `pluginFromLibrary(path)` | `CCORDIS_PLUGIN_DEF` 导出（§5） | 独立编译分发的模块 | 需要与内核头严格同版本（§5.3） |

**心智模型**：`apply`/构造函数在**依赖满足时**被调用；依赖消失时你注册的一切（服务/订阅/子插件）被自动回收；依赖恢复时**重新调用**（新实例、新子作用域）。插件要写成"可重入的装配过程"，而不是"一次性初始化"。

### 命名规范

| 类别 | 约定 | 示例 |
| --- | --- | --- |
| 服务 id | `<域>.<名>` | `model.core` `theme.manager` `view.waterfall` `dsp.bpf` `conn.vpx` |
| 事件 | `<域>.<动作过去式>` | `band.changed` `cmd.ack` `cmd.timeout` `app.ready` |
| 通道 | `<域>.<流名>` | `spectrum.frames` `spectrum.filtered` `iter.bands` |
| 插件 id | 与主服务同域 | `source.simulator` 提供 `signal.source` |

---

## 1. 插件类型总览

| 类型 | provides | requires | 线程面 | 典型宿 |
| --- | --- | --- | --- | --- |
| **GUI 视图** | `view.<n>`（QWidget 服务） | `model.core` `theme.manager` | GUI | 宿主/内置 |
| **布局容器** | `ui.container.<路径>` + `view.<id>`（容器即视图，可嵌套） | 父容器服务（嵌套时） | GUI | 宿主/内置/动态 |
| **数据处理** | 下一级通道（消费 A 发布 B） | `model.core` + 上游通道 | GUI（pump 中）+ 可选工作线程 | 内置或动态 |
| **数据源** | `spectrum.frames` 等首级通道 | `conn.*` 或无 | **生产线程（post）** | 内置或动态 |
| **协议连接** | `vpx.client` `device.command` | 无 | GUI 登记 / IO 执行 | 内置（与 SDK 绑定） |
| **核心服务** | `model.core` 等 | 无 | GUI | **宿主直注册，不插件化**（含专利载荷，见 §8 红线） |

---

## 2. GUI 插件（视图类）

**要点**：GUI 插件的编译单元属于**宿主侧**（可以 include Qt——依赖红线约束的是内核，不是宿主侧插件）；视图对象所有权归 **Qt 父子体系**，注册进内核的是**非拥有**句柄（评审 F5 规范）。

```cpp
// view_waterfall.cpp —— 类形态（析构由拆撤触发，配合 Qt parent 双保险）
#include "ccordis/Context.h"
#include <QWidget>

class WaterfallPlugin {
public:
    WaterfallPlugin(ccordis::Context &ctx, const ccordis::Value &cfg)
        : m_view(new WaterfallWidget(/*parent=*/nullptr))   // 挂布局时再 setParent
    {
        // 1) 依赖注入: 单一事实源 + 主题
        auto model = ctx.inject<SignalModel>("model.core");
        auto theme = ctx.inject<ThemeManager>("theme.manager");
        m_view->setModel(model.get());
        m_view->applyTheme(theme->current());

        // 2) 非拥有注册: shared_ptr 空删除器, 销毁权在 Qt 父子体系
        ctx.provide<QWidget>("view.waterfall",
            std::shared_ptr<QWidget>(m_view, [](QWidget *) {}));

        // 3) 控制面: 主题/频段事件
        ctx.on("theme.changed", [this](const Value &v) {
            m_view->applyTheme(v.at("name").toString());
        });
        ctx.on("band.changed", [this](const Value &v) {
            m_view->setBand(v.at("min").toDouble(), v.at("max").toDouble());
        });

        // 4) 数据面: 只存最新帧, paint 时读（视图按自身节奏渲染）
        ctx.onBlob("spectrum.frames", [this](const ccordis::Blob &frame) {
            m_view->setLatestFrame(frame);      // 零拷贝存 Blob(引用计数保活)
            m_view->update();                    // Qt 调度重绘
        });
    }
private:
    QPointer<WaterfallWidget> m_view;
};

// 宿主装载（MainWindow 或装配插件内）:
ctx.plugin<WaterfallPlugin>("view.waterfall",
                            Value::object({{"historyRows", 600}}),
                            {"model.core", "theme.manager"});
```

**规则**：
- 视图**必须能缺省运行**（数据未到画空态）——通道 latest-wins 语义下你只管消费到的；
- 瀑布图等需要**历史**的视图：消费时把行数据**拷入自己的环形缓存**（Blob 是不可变快照，别想改它）；
- 永远不要在视图里阻塞等数据；慢了就丢帧（通道 stats 可观测）。

### 2.1 布局容器插件：子元素装配与嵌套协议

容器（分割器/标签页/停靠区）**本身也是插件，且同时是容器和视图**——对上提供 `ui.container.<路径>` 服务供子元素挂入，对下作为 `view.<id>` 挂入父容器。**嵌套由此免费获得**：split 里放 tabs、tabs 里放视图，递归成立。

**容器契约**（宿主侧公共头；动态插件跨 DSO 时 `CCORDIS_INTERFACE` 锚定）：

```cpp
class CCORDIS_INTERFACE IUiContainer {
public:
    virtual ~IUiContainer() = default;
    /** 子视图请求挂入; placement 为布局协商元数据(slot/stretch/tab名等) */
    virtual void attach(QWidget *view, const ccordis::Value &placement) = 0;
    virtual void detach(QWidget *view) = 0;
    virtual std::string containerPath() const = 0;      // 如 "ui.container.split.main"
};
```

**通信设计（四个通道，各司其职）**：

| 通道 | 载体 | 用途 |
| --- | --- | --- |
| **装配**（点对点） | `inject<IUiContainer>` + `attach/detach` | 子元素挂入/摘出容器 |
| **布局协商** | `attach` 的 `placement` Value（`{"slot":0,"stretch":3}` / `{"tab":"瀑布"}`） | 声明期望位置，容器解释执行 |
| **变更广播**（一对多） | 事件 `ui.layout.changed`（载荷含容器路径） | 分割条拖动/页切换/浮动静默视图刷新 |
| **持久化** | `describeLayout()` 导出 Value 子树 → 宿主存取 | 布局保存/恢复（M3） |

**子视图侧**（构造时注入目标容器并挂入）：

```cpp
class LabelView {   // 类形态; 依赖由 requires 声明, 容器缺席则自动 Deferred
public:
    LabelView(ccordis::Context &ctx, const ccordis::Value &cfg) {
        m_w = new QLabel(QString::fromStdString(cfg.at("title").toString("view")));
        m_w->setObjectName(QString::fromStdString(cfg.at("id").toString()));
        ctx.inject<IUiContainer>("ui.container." + cfg.at("container").toString("main"))
            ->attach(m_w, cfg.at("placement"));
        ctx.provide<QWidget>("view." + cfg.at("id").toString("label"),
                             std::shared_ptr<QWidget>(m_w, [](QWidget *) {})); // 非拥有
    }
private:
    QLabel *m_w;
};
// 宿主装载: 依赖是"配置驱动"的——requires 按配置中的目标容器组装
ctx.plugin<LabelView>("view.a",
    Value::object({{"id","a"},{"container","split.main"},
                   {"placement", Value::object({{"stretch",3}})}}),
    {"ui.container.split.main"});        // ← 容器上线即自动激活装配
```

**容器侧**（分割器示例——注意构造顺序的协议规则）：

```cpp
class SplitContainer {
public:
    SplitContainer(ccordis::Context &ctx, const ccordis::Value &cfg) {
        const std::string id = cfg.at("id").toString("split");
        auto *s = new QSplitter(cfg.at("horizontal").toBool() ? Qt::Horizontal
                                                              : Qt::Vertical);
        s->setObjectName(QString::fromStdString(id));
        // 协议规则①: 先挂入父容器, 再 provide 自己的服务 —— provide 的瞬间
        // 子视图会重入激活(依赖状态机), 必须保证它们 attach 时本容器已在父级树上
        ctx.inject<IUiContainer>("ui.container." + cfg.at("parent").toString("main"))
            ->attach(s, cfg.at("placement"));
        ctx.provide<IUiContainer>("ui.container." + id,
                                  std::make_shared<Facade>(s, "ui.container." + id));
        ctx.provide<QWidget>("view." + id,
                             std::shared_ptr<QWidget>(s, [](QWidget *) {}));
        // 变更广播: 布局变化通知全树
        QObject::connect(s, &QSplitter::splitterMoved, [&ctx, id](int, int) {
            ctx.emitEvent("ui.layout.changed",
                          Value::object({{"container", "ui.container." + id}}));
        });
    }
    // Facade: attach → splitter->addWidget + setStretchFactor(placement)
};
```

**宿主根容器**：MainWindow 在引导时直注册 `ui.container.main`（非插件），即整棵装配树的根。

**生命周期全由内核依赖状态机托管**（offscreen 实测验证，装配冒烟存档）：

1. **顺序无关**：先装视图后装容器 → 视图 `Deferred`；容器 `provide` 瞬间子视图重入激活自动挂入；
2. **嵌套**：`view.c` requires `ui.container.tabs.right`，tabs requires `ui.container.split.main`，split requires `ui.container.main`——链条即树，`findChild` 从根可达最深视图；
3. **拆除级联**：卸载中间容器 → 其 `ui.container.*`/`view.*` 服务撤销 → 整棵子树自动回 `Deferred`（Qt 父子体系同步回收控件）；
4. **恢复重建**：容器重新装载 → 子树全部自动重新装配（新实例）。

**布局即配置**（整棵树可外置）：

```json
{ "plugins": [
  {"plugin":"ui.split","options":{"id":"split.main","parent":"main","horizontal":true}},
  {"plugin":"ui.tabs", "options":{"id":"tabs.right","parent":"split.main"}},
  {"plugin":"view.spectrum", "options":{"container":"split.main","placement":{"slot":0,"stretch":3}}},
  {"plugin":"view.waterfall","options":{"container":"tabs.right","placement":{"tab":"瀑布"}}}
]}
```

**容器专属规则**：
- 路径即标识：`ui.container.<id>` 全局唯一；`parent` 链**禁止成环**（装载时校验路径链，含自身即拒绝）；
- `attach/detach` 只在 GUI 线程（插件构造/事件回调内天然满足）；
- 控件所有权归 Qt 父子体系，内核只持非拥有句柄（同 §2 F5 规范）；容器销毁 → 子控件随 Qt 树回收 → 子插件重新激活时创建**新实例**。

---

## 3. 数据处理插件（DSP/数据面）

数据面流水线模式：**消费上游通道 → 变换 → 发布下游通道**。发布走 `publish`（宿主线程同步扇出）；源端（设备线程）才用 `post`。

```cpp
// dsp_bandpass.cpp —— 带通滤波: spectrum.raw → spectrum.filtered
#include "ccordis/Context.h"

class BandPassPlugin {
public:
    BandPassPlugin(ccordis::Context &ctx, const ccordis::Value &cfg)
        : m_out(ctx.channel("spectrum.filtered")),
          m_pool(ctx.blobPool())                      // 树共享的根池
    {
        m_loMHz  = cfg.at("loMHz").toDouble(30.0);
        m_hiMHz  = cfg.at("hiMHz").toDouble(3000.0);
        // 可选: 依赖服务
        // auto model = ctx.inject<SignalModel>("model.core");
        ctx.onBlob("spectrum.raw", [this](const ccordis::Blob &in) {
            ccordis::Blob out = ccordis::Blob::allocate(in.size(), m_pool);
            process(in, out);                         // in/out 均 64B 对齐(SIMD 友好)
            m_out->publish(out);                      // 同步扇出给下游 N 视图/插件
        });
        // 滤波参数可被控制面事件调
        ctx.on("dsp.bpf.tune", [this](const Value &v) {
            m_loMHz = v.at("loMHz").toDouble(m_loMHz);
            m_hiMHz = v.at("hiMHz").toDouble(m_hiMHz);
        });
    }
private:
    void process(const ccordis::Blob &in, const ccordis::Blob &out) {
        // 只读 in（不可变契约），只写 out（尚未发布）
        const float *src = static_cast<const float *>((const void *)in.data());
        float *dst = static_cast<float *>((void *)out.data());
        // ... SIMD 滤波 ...
    }
    std::shared_ptr<ccordis::BlobChannel> m_out;
    ccordis::BlobPool &m_pool;
    double m_loMHz = 30, m_hiMHz = 3000;
};

ctx.plugin<BandPassPlugin>("dsp.bpf",
                           Value::object({{"loMHz", 30.0}, {"hiMHz", 3000.0}}),
                           {});                        // 无服务依赖, 仅依赖通道
```

**源端插件**（设备线程生产，如 vpxproto 帧解析回调里）：

```cpp
// 生产纪律: 单写者在 post 之前完成全部写入(§9 内存模型)
void VpxProducer::onSpectrumFrame(const RawFrm &frm)    // IO/设备线程
{
    ccordis::Blob blob = ccordis::Blob::allocate(frm.bytes, m_ctx.blobPool());
    std::memcpy((void *)blob.data(), frm.data, frm.bytes);
    m_ctx.channel("spectrum.frames")->post(std::move(blob));  // 任意线程安全
}   // post 后字节冻结; 宿主 pumpHook→pump 投递
```

**规则**：
- 消费者做多级流水线时注意**扇出即广播**：你的输出会发给所有下游，别假设单消费者；
- 帧 32B 头（§9.3：magic/seq/timestampNs）由首级生产者填，`seq` 跳变即丢帧观测点；
- 需要**异步重计算**（如瀑布重采样）可自开工作线程，但回发结果必须回到宿主线程 `publish`（工作线程只许 `post`）。

---

## 4. 应用层协议插件（conn.* / 设备命令）

职责：把"类型化命令服务"暴露给全树，内部完成**去抖 → requestId → 超时跟踪 → 协议编码 → IO 发送 → 回程匹配**（全链路规则见设计 §9.2）。

```cpp
// conn_vpx.cpp —— 类形态; provide device.command + vpx.client
#include "ccordis/Context.h"
#include "ccordis/RequestTracker.h"

// 命令契约(公共头, 跨DSO时加 CCORDIS_INTERFACE)
class CCORDIS_INTERFACE DeviceCommandService {
public:
    virtual ~DeviceCommandService() = default;
    virtual std::uint64_t setBand(double minMHz, double maxMHz) = 0;  // 立即返回
    virtual std::uint64_t queryCali() = 0;
};

class CmdFacade : public DeviceCommandService {
public:
    CmdFacade(ccordis::Context &ctx, std::shared_ptr<VpxDeviceClient> client)
        : m_ctx(ctx), m_client(std::move(client)) {}

    std::uint64_t setBand(double lo, double hi) override {   // GUI 线程
        m_pending = {lo, hi};                                 // 尾值合并(去抖)
        m_dirty = true;
        return 0;                                             // opId 在真正下发时分配
    }
    void onHostTick() {                                       // 宿主 100ms 节拍(GUI线程)
        if (m_dirty && m_debounceElapsed()) flushBand();
        for (auto req : m_tracker.tick(std::chrono::steady_clock::now()))
            onTimeout(req);
    }
    void onHostDisconnect() {                                 // 心跳丢失: fail-fast
        for (auto req : m_tracker.expireAll())
            m_ctx.emitEvent("cmd.timeout", Value::object(
                {{"req", (std::int64_t)req}, {"reason", "disconnected"}}));
    }
    void onResponse(std::uint64_t req, int code) {            // QueuedConnection 跳回GUI后
        using O = ccordis::RequestTracker::Outcome;
        const O oc = m_tracker.complete(req);
        if (oc == O::Acked)
            m_ctx.emitEvent("cmd.ack", Value::object(
                {{"req", (std::int64_t)req}, {"code", code}}));
        else if (oc == O::LateAck)
            m_ctx.emitEvent("cmd.late", Value::object({{"req", (std::int64_t)req}}));
        // Unknown(含 fail-fast 后的迟到): 静默忽略
    }
private:
    void flushBand() {                                        // GUI 线程
        const std::uint64_t req = m_nextReq++;
        m_tracker.track(req, m_timeout /*SET_BAND 500ms*/);
        m_client->postSetBand(m_pending.first, m_pending.second);
        // ↑ 打包POD命令→strand→IO线程编码发送(GUI零阻塞, §9.2 军规2)
        m_dirty = false;
    }
    void onTimeout(std::uint64_t req) {                       // 幂等命令: 重试≤2次(换新req)
        if (++m_attempts[req] <= 2) { m_tracker.forget(req); flushBand(); }
        else m_ctx.emitEvent("cmd.timeout",
                             Value::object({{"req", (std::int64_t)req}}));
    }
    ccordis::Context &m_ctx;
    std::shared_ptr<VpxDeviceClient> m_client;
    ccordis::RequestTracker m_tracker;
    std::pair<double, double> m_pending{30, 6000};
    bool m_dirty = false;
    std::uint64_t m_nextReq = 1;
    std::map<std::uint64_t, int> m_attempts;
    std::chrono::milliseconds m_timeout{500};
};

class VpxClientPlugin {
public:
    VpxClientPlugin(ccordis::Context &ctx, const ccordis::Value &cfg)
        : m_client(std::make_shared<VpxDeviceClient>())
    {
        m_client->connect(cfg.at("host").toString("127.0.0.1"),
                          (std::uint16_t)cfg.at("port").toInt(10097));
        ctx.provide<DeviceCommandService>("device.command",
                                          std::make_shared<CmdFacade>(ctx, m_client));
        ctx.provide<VpxDeviceClient>("vpx.client", m_client);
        // 数据上行: IO线程回调→Blob→post(§3 源端模式)
        // 回程命令响应: IO解析→QueuedConnection→GUI→facade->onResponse
    }
private:
    std::shared_ptr<VpxDeviceClient> m_client;
};

ctx.plugin<VpxClientPlugin>("conn.vpx",
    Value::object({{"host", "127.0.0.1"}, {"port", 10097}}), {});
```

**调用方（视图/任意插件）只需三行**：

```cpp
if (auto cmd = ctx.inject<DeviceCommandService>("device.command"))
    cmd->setBand(30, 6000);                       // 非阻塞
ctx.on("cmd.ack",   [](const Value &v) { /* 回显成功 */ });
ctx.on("cmd.timeout", [](const Value &v) { /* 回滚乐观UI/提示重试 */ });
```

**协议映射备忘**（本仓拓扑：demo↔scouter↔设备）：`0x9201` 频谱 → `spectrum.frames` 通道；`0xb441/0xb442` 迭代频段 → `iter.bands`；`0x95C0` 校准查询 → `DeviceCommandService::queryCali`；`0x9701` 心跳 → 断连判定 → `onHostDisconnect` fail-fast。

#### 命令封套 schema 约定（协议插件 input contract）

控制面命令统一以 `Value` 对象（"封套"）传递。为避免不同协议插件对同一命令的字段名/类型解释不一致，约定如下 schema——协议插件在入口处按此校验，`DeviceCommandService` 实现类负责翻译到具体协议码。

```
通用封套骨架:
{ "type":   "<命令名>",             // 必填, string — 命令标识(连接插件翻译到协议码)
  "id":     <requestId>,            // 必填, int64 — 调用方关联(去重/匹配 ack)
  "params": { ... } }               // 必填, object — 命令参数(见各命令表)
```

| 命令 type | 幂等 | params 必填字段 | 选填字段 | 说明 |
| --- | --- | --- | --- | --- |
| `SET_BAND` | 是 | `min` (double, MHz), `max` (double, MHz) | — | 设置扫描频段 |
| `START_SCAN` | 是 | — | `mode` (string) | 启动扫描 |
| `STOP_SCAN` | 是 | — | — | 停止扫描 |
| `QUERY_CALI` | 是 | — | — | 查询校准状态 |
| `SET_GAIN` | 是 | `gainDb` (double) | `chan` (int) | 增益控制 |
| `TRIG_FF` | 否 | `freqMHz` (double), `spanMHz` (double) | `durMs` (int) | 触发频率微调(非幂等: 每次触发新动作) |
| `USER_CMD` | — | `subType` (string) | 任意 | 用户扩展(子类型自定义 params) |

**校验规则**：
- `type` 缺失或未知名 → 拒收，emit `cmd.nak` `{id, code:"UNKNOWN_TYPE"}`；
- `id` 缺失 → 拒收，无法关联 ack；
- `params` 中必填字段缺失或类型不符 → 拒收，emit `cmd.nak` `{id, code:"BAD_PARAMS", field:"<字段名>"}`；
- 幂等命令重复 `id` → 去重（返回缓存的 ack），不重复下发；
- 非幂等命令每次分配新 `id`。

**回程事件约定**（协议插件 emit）：

| 事件 | 载荷 | 含义 |
| --- | --- | --- |
| `cmd.ack` | `{id, code:0, [result:{...}]}` | 命令执行成功（`result` 可选，如校准数据） |
| `cmd.nak` | `{id, code:"<原因>"}` | 命令被拒（参数错/未知类型/设备忙） |
| `cmd.timeout` | `{id, reason:"<disconnected\|retry-exhausted>"}` | 超时（含断连 fail-fast） |
| `cmd.late` | `{id}` | 迟到响应（超时后到达，仅日志/对账，不改 UI 状态） |

> 调用方订阅 `cmd.ack / cmd.nak / cmd.timeout` 驱动 UI 回显；`cmd.late` 仅用于排障日志。

---

## 5. 动态插件（.so）打包发布

> 内核已迁移为独立 CMake 项目 `code/ccordis/`（公共头 `include/ccordis/`，
> 构建 `cmake -S code/ccordis -B code/ccordis/build && cmake --build code/ccordis/build -j`，
> 产物 `libccordis.so.2` + 示例插件 `examples/hello/libccordis_hello.so`）。

### 5.1 源码骨架

```cpp
// ccordis/examples/hello/hello_plugin.cpp —— 纯 C++17, 不依赖 Qt
#include <ccordis/Plugin.h>   // 内核头(与宿主严格同版本, 见 5.3)
#include <ccordis/Context.h>

class HelloPlugin : public ccordis::IPlugin {
    void apply(ccordis::Context &ctx, const ccordis::Value &cfg) override {
        ctx.on("app.ready", [](const ccordis::Value &) { /* ... */ });
        // 注册写法与内置插件完全一致: provide/inject/watch/on/onBlob/...
    }
};
CCORDIS_PLUGIN_DEF(HelloPlugin, "demo.hello")                  // 无依赖
// CCCORDIS_PLUGIN_DEF_REQ(HelloPlugin, "demo.hello", "model.core")  // 声明依赖
```

跨 DSO 服务接口（如 `DeviceCommandService`）必须加 `CCORDIS_INTERFACE`（`Export.h`）锚定唯一 typeinfo（§7.2 条款）。

### 5.2 CMake 工程模板

```cmake
# examples/hello/CMakeLists.txt（实库参考 ccordis/examples/hello/）
add_library(ccordis_hello MODULE hello_plugin.cpp)
target_compile_features(ccordis_hello PRIVATE cxx_std_17)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(ccordis_hello PRIVATE -fvisibility=hidden)  # 只导出 ccordis_plugin_entry
endif()
target_link_libraries(ccordis_hello PRIVATE ccordis::ccordis)   # 产品态共链内核
```

### 5.3 发布三条铁律（实测教训，§7.2/§7.3）

1. **同头文件版本**：内核头改了（哪怕只加成员）必须重编全部插件——陈旧 .so 会静默写错成员槽位（症状：服务撤销清单错位、卸载后注册表残留、拆除崩溃）；
2. **产品态共链内核**：插件与宿主链接同一 `libccordis.so.2`（soname 跟随 `CCORDIS_ABI_VERSION`），运行期共享唯一内核实例；宿主不再内嵌内核源码，也无需 `-rdynamic`；
3. **装载与卸载**：宿主 `ctx.pluginFromLibrary(".../libccordis_hello.so")`（或配置 `lib:` 前缀）；管理卸载 `ctx.unload(name)` 只杀服务与实例，库驻留到宿主析构（§7.3 退役库纪律，别自己 dlclose）。

---

## 6. 配置驱动装载

```json
{ "plugins": [
    { "plugin": "conn.vpx",        "options": { "host": "127.0.0.1", "port": 10097 } },
    { "plugin": "dsp.bpf",         "options": { "loMHz": 30.0, "hiMHz": 3000.0 } },
    { "plugin": "view.waterfall",  "options": { "historyRows": 600 } },
    { "plugin": "lib:plugins/libccordis_hello.so" }
] }
```

宿主把 JSON 转成 `Value` 后 `ctx.loadConfig(doc)`；未知名告警跳过不致命。插件侧读取一律 `cfg.at("键").toXxx(缺省)`——**每个配置项都要有缺省值**，保证零配置可跑。

---

## 7. 测试你的插件

- **单元**：`Context` 即测试夹具——先 `provide` fake 依赖，再装载插件，断言其 `provides`/事件/通道输出（服务 id 契约使 mock 与真实实现互换，无需内核内建 mock 框架）；
- **依赖重启**：装载 → unload 依赖提供者 → 断言你的插件回 `Deferred` 且资源回收 → 重新装载 → 断言重入（重启计数）；
- **数据面**：post 固定帧 → pump → 断言输出 Blob 指针同一性（零拷贝）与内容；
- **动态**：T8/T17 往返（装载→注入→卸载→无残留），每次**宿主与插件一起重编**。

---

## 8. 常见陷阱（全部实测踩过）

| 陷阱 | 后果 | 规避 |
| --- | --- | --- |
| `f().asArray()` 链式取指针 | 悬垂指针遍历垃圾（本会话咬了三次） | 先存具名 `Value` 再 `asArray` |
| Blob 发布后继续写 | 数据竞争（不可变契约破坏） | post/publish 前完成全部写入 |
| 在 IO 线程 `publish`/`on`/`provide` | 破坏单线程模型 | IO 线程只许 `post`；回程先 QueuedConnection |
| pumpHook 里直接 QTimer/控件 | 线程亲和违规 | hook 只做唤醒调度（A1 契约） |
| 插件用旧内核头编译 | 静默成员错位 → 拆除崩溃 | 同头文件版本铁律（§5.3） |
| `emit` 做方法名 | Qt 宏冲突编译炸 | 内核已更名 `emitEvent`；插件侧同理 |
| `requires` 做标识符 | C++20 起关键字 | 用 `required`/`reqs` |
| 持有注入指针当永久所有权 | "失维护"服务 | 用 `watch` 跟随上下线，消失即停用 |
| 容器 provide 早于挂入父容器 | 子视图 attach 到"悬空"容器（树外） | 协议规则①：先 attach 进父容器再 provide（§2.1） |
| 容器 parent 链成环 | 无限递归装配 | 装载时校验路径链，含自身即拒绝 |
| 把调度器插件化 | 触碰专利载荷红线 | NewTargetScheduler 等核心时序**不插件化**（设计 §11.4） |

## 9. 发布前检查清单

- [ ] 插件 id / provides / 通道名符合命名规范，`PluginMeta.provides` 与实际一致
- [ ] 所有配置项有缺省值；零配置可装载
- [ ] 依赖只声明真正需要的 service id（不依赖具体插件名）
- [ ] apply/构造函数可重入（依赖重启无残留、无泄漏）
- [ ] dispose/析构不抛异常、不回调内核
- [ ] 数据面：帧头 seq 连续可查；慢消费者时行为明确（丢旧/缓存上限）
- [ ] 命令面：非阻塞 + ack/timeout 事件齐备；断连 fail-fast 已接入
- [ ] 动态版：与宿主同头文件同批构建；`-fvisibility=hidden` + `CCORDIS_INTERFACE` 接口
- [ ] 冒烟全绿：装载/重启/卸载/拓扑断言 + （动态）T8/T17 往返
