# CORDIS 理念插件系统完整设计方案 (ccordis kernel, v2)

> 迁移说明 (2026-09-15): 本文档随 ccordis 独立化从 `SpectrumExplorerDemo/doc/` 迁入本项目 `doc/`。文中旧路径对照: 内核代码 `ccordis/` → `include/ccordis/`(公共头) + `src/`(实现); `plugins/hello` → `examples/hello`; 生命周期细节见 `plugin-lifecycle.md`。

> 状态: 设计稿 v2.1 (2026-09-14，**内核更名 cordis→ccordis**，意即 "C++ 的 cordis"——理论出处仍是 CORDIS/TS；此前 v2 为去 Qt 化修订)
> 范围: `SpectrumExplorerDemo` 插件系统内核 `ccordis/` 的架构、契约、生命周期、集成与测试方案
> 理论蓝本: [CORDIS](https://cordis.moe/zh-CN/guide/essential/index.html)（MIT）
> 语义参考: CppMicroServices（Apache-2.0，OSGi-like）—— 见 `ccordis-selection-report.md`
> 关联: `PATENT_MAPPING.md`（专利载荷边界，§11.4）、`doc/manual-scout-design.md`（P1 导航条四平面，§11.3）
>
> **v2 修订要点**：内核接口与 Qt 彻底解耦——只依赖 **C++17 标准库 + 最小跨平台封装**（动态库加载）。Qt 退至宿主适配层。

---

## 0. 依赖红线（本设计的最高约束）

**内核（`ccordis/`）公共头文件只允许包含 C++17 标准库头**：

```
<string> <vector> <map> <functional> <memory> <mutex> <typeindex>
<variant> <cstdint> <utility> <algorithm> <optional> <filesystem>
```

- **禁止**出现：Qt 任何头、Boost、操作系统 SDK 头（`windows.h`/`dlfcn.h` 只允许出现在 `SharedLibrary.cpp` 一个翻译单元内）；
- **JSON 不引入解析器**：内核自带 `ccordis::Value`（`std::variant` 值类型）表达配置/载荷；文件解析与宿主格式（QJsonObject 等）的互转由**宿主适配层**负责；
- **日志注入**：`ccordis::setLogger(fn)` 回调，默认写 stderr——内核不依赖任何日志库；
- **字符串编码**：接口层一律 `std::string`（UTF-8 约定）；
- 由此内核可在**任何 C++17 宿主**（Qt / 纯 C++ / 服务进程 / 测试）中复用，插件 ABI 也不再捆绑 Qt 运行时。

**分层图**：

```
┌─────────────────────────────────────────────────────┐
│ 宿主应用 (Qt / 纯C++ / ...)                          │
│   ├─ adapters/qt/  Qt桥: QString↔string, QJson↔Value │  ← 宿主侧，非内核
│   └─ 业务插件 (视图/数据源/...)                        │
├─────────────────────────────────────────────────────┤
│ ccordis/ 内核（纯 C++17）                              │
│   Context · ServiceRegistry · EventBus · Plugin      │
│   Value(variant配置) · SharedLibrary · Export        │
├─────────────────────────────────────────────────────┤
│ OS 触点: dlopen / LoadLibrary —— 仅 SharedLibrary.cpp│
├─────────────────────────────────────────────────────┤
│ 语义参考: CppMicroServices（Apache-2.0，不链接）      │
└─────────────────────────────────────────────────────┘
```

**语言标准**：内核为 C++17（`std::variant`/`std::filesystem` 是地基）。宿主接入时应用工程需 `CONFIG += c++17`（qmake）——平台支持矩阵见前期调研：GCC≥9 / Clang≥7 / VS2019 16.x / Xcode≥11 起全量可用，本机 g++ 13.3 默认即 gnu++17，实测零障碍。

---

## 1. 背景与目标

选型结论（详见调研报告）：以 CppMicroServices 为语义参考，仓内实现 CORDIS 语义内核。v1 原型以 Qt 类型为地基（QString/QJsonObject/QLibrary），v2 依新约束重构为纯 C++17——**插件系统是长生命周期基础设施，不得与 GUI 框架共命运**。

设计目标：

1. 忠实移植 CORDIS 五要素：插件三形态、Context 服务容器、依赖声明与重启、嵌套作用域、配置装载与具名可视化；
2. C++ 原生表达：RAII 取代显式 `dispose()`；
3. **内核零框架依赖**（§0 红线），宿主以薄适配层桥接；
4. 对现有 demo 叠加式改造，默认数据链路与专利时序零回归。

非目标：插件版本协商、跨进程服务、脚本宿主、已编译代码热替换。

---

## 2. CORDIS → C++17 概念映射

| CORDIS (TS) | cordis v2 (纯 C++17) | v1（已废弃） |
| --- | --- | --- |
| 插件·函数形态 `function(ctx, options)` | `ctx.plugin(name, meta, applyFn, config)`，`ApplyFn = std::function<void(Context&, const Value&)>` | QJsonObject |
| 插件·类形态 `class { constructor(ctx, options) }` | `ctx.plugin<T>(name, config, required)`，要求 `T(Context&, const Value&)` | 同 |
| 插件·对象形态 `{ apply }` | `ctx.plugin(name, IPlugin*, config, required)` | 同 |
| `Context` 树形作用域 | `ccordis::Context`（子作用域随插件启动创建） | 同 |
| `ctx.provide/inject` | `provide<T>(id, std::shared_ptr<T>)` / `inject<T>(id)`，id 为 `std::string` | QString |
| 反应式注入 | `watch<T>(id, cb)` / `watchAny(id, cb)` | 同 |
| `ctx.on / ctx.emit` | `on(event, handler)` / **`emitEvent`**（保留 v1 改名：Qt 宿主仍持有 `emit` 宏，内核头必须能在 Qt 头之后被包含） | `emit`（撞宏，废弃） |
| 事件载荷 | `Value`（对象类型） | QVariantMap |
| `ctx.middleware` | `middleware(mw, prepend)` + `run(payload)` | 同 |
| `requires` | `PluginMeta::requires`（`std::vector<std::string>`；形参名用 `required`，避开 C++20 关键字） | QStringList |
| 配置驱动 | `loadConfig(const Value&)`，`"lib:"` 前缀装载动态库 | QJsonObject |
| 具名插件可视化 | `topology()` → `Value` 树 | QJsonObject |
| 动态模块（npm 包） | `SharedLibrary`（dlopen/LoadLibrary 封装）+ `ccordis_plugin_entry()` ABI | QLibrary |
| 配置/载荷值 | `ccordis::Value`（`std::variant` 递归值类型，§4.1） | QJson* 家族 |

---

## 3. 总体架构

```
              ┌────────────────────────────────────────────┐
              │     应用根作用域  ccordis::Context            │
              │  (持有共享 ServiceRegistry + EventBus)       │
              └────────────────────────────────────────────┘
                 │ plugin()          │ plugin()       │ pluginFromLibrary()
        ┌────────┴───────┐  ┌────────┴───────┐  ┌────┴─────────────────┐
        │ LoadRecord A   │  │ LoadRecord B   │  │ LoadRecord C         │
        │ state=Active   │  │ state=Deferred │  │ state=Active         │
        │ ┌────────────┐ │  │  requires 未满足│  │  PluginDef (C-ABI)   │
        │ │ 子 Context │ │  │  (依赖哨兵在哨) │  │ ┌────────────┐      │
        │ │ provide()  │ │  └────────────────┘  │ │ 子 Context │      │
        │ │ on()/watch │ │                      │ │ +SharedLib │      │
        │ │ 子插件...   │ │                      │ └────────────┘      │
        │ └────────────┘ │                      └──────────────────────┘
        └────────────────┘
  ═════════════════════════════════════════════════════════════════════
   ServiceRegistry (type_index,id)→shared_ptr<void>   EventBus name→[handler]
   provide/revoke 时触发 watcher（晚绑定/依赖重启的数据面）
```

LoadRecord 为插件在宿主中的档案（元数据/状态/子作用域/依赖哨兵/激活期资源/宿主期资源）；子作用域是插件沙箱账本，apply 期间的一切注册记在其名下，拆除时逆序回收；Registry 与 EventBus 全树共享。

---

## 4. 模块设计

### 4.1 `ccordis/Value.h` — 配置与载荷值类型（header-only）

JSON 形值的 `std::variant` 递归实现：

```cpp
class Value {
public:
    using Array  = std::vector<Value>;
    using Object = std::map<std::string, Value>;      // 键唯一（JSON 语义）
    using Storage = std::variant<std::monostate,      // null
                                 bool, std::int64_t, double,
                                 std::string, Array, Object>;
    // 构造：Value() / Value(nullptr) / bool / 整型 / 浮点 / const char* /
    //        std::string / Array / Object
    // 判型：isNull/isBool/isInt/isDouble/isString/isArray/isObject
    // 取值：toInt(dflt)/toDouble(dflt)/toString()/asArray()/asObject()
    // 对象访问：at(key)（缺键返回 null Value）/ has(key)
    // ⚠ asArray()/asObject() 返回指向内部存储的指针——禁止链式调用在
    //   临时 Value 上取值（`f().asArray()` 即悬垂）。正确姿势：先以具名
    //   Value 保存再取指针（开发中此坑出现三次，遂成文）。
    // 构造助手：Value::object({{"a",1},{"b","x"}}) / Value::array({...})
};
```

- 递归 variant 依赖 `std::vector` 对不完整类型的 C++17 支持；
- **只做值语义，不做解析**：`{"rateHz", 5}` 直接程序化构造；从 JSON 文本到 Value 属宿主适配层（Qt 侧 `QJsonObject`→`Value`，非 Qt 宿主可自选 nlohmann/json 转 Value）；
- 事件载荷 `Payload` 是 `Value` 的别名（约定为对象类型）。

### 4.2 `ccordis/EventBus.h` — 同步事件总线

- `Handler = std::function<void(const Value&)>`；事件名 `std::string`；
- `on()` 返回 RAII `ScopedConnection`（可移动不可拷贝，析构退订）；`Context::on()` 将连接记入作用域实现 CORDIS dispose 自动化；
- `emitEvent()` 同步、单线程分发；**快照后回调**，处理器内退订/再发布安全；
- 内部互斥仅为快照一致性；跨线程分发由宿主先跳回宿主线程（Qt 侧即 `QueuedConnection` 模式）；
- **异常策略（评审 A3）**：处理器约定不抛；内核在分发边界逐个 catch-all 并经 logger 告警，单个坏处理器不中断其余订阅者、不穿透拆除级联；
- **顺序承诺（评审 F9）**：同一事件按**订阅序（FIFO）**分发（实现即追加序快照）。

### 4.3 `ccordis/ServiceRegistry.h` — 类型化服务注册表

- 服务键 `(std::type_index 具体类型, std::string id)` → `shared_ptr<void>` + owner 指针（作用域拆除定向撤销）；
- `inject` 严格类型校验（id 命中类型不符 → nullptr）；
- `watch(id, cb)` 注册即回调当前状态（nullptr=缺席），此后每次上/下线转换回调——晚绑定与依赖重启的公共数据面；
- 同 id 首个提供方生效，同 owner 重复 provide 原位替换；
- **宿主 GUI 类型（如 QWidget）作为服务**：`provide<QWidget>(id, w)` 由宿主 TU 使用，内核只需 `<typeindex>`，无需知晓 QWidget——这是"内核不依赖 Qt 而宿主仍能插件化视图"的关键机制。

### 4.4 `ccordis/Plugin.h` — 插件契约（跨 ABI）

```cpp
class IPlugin {
public:
    virtual void apply(Context &ctx, const Value &config) = 0;
    virtual void dispose();                                  // 可选
};

struct PluginMeta {
    std::string name;                 // 唯一 id，如 "source.simulator"
    std::string label;                // 人读名
    std::vector<std::string> requires;// 依赖服务 id
    std::vector<std::string> provides;// 计划提供（文档性质）
};

struct PluginDef {                    // C-ABI：纯指针，无 C++ 运行时对象跨界
    const char *name;
    const char *label;                // 可为 nullptr
    const char *const *requires;      // nullptr 结尾，可为 nullptr
    IPlugin *(*create)();
    void (*destroy)(IPlugin *);
};
```

导出宏在 `ccordis/Export.h`（内核中唯一触碰符号可见性的地方）：

```cpp
#if defined(_WIN32)
#  define CCORDIS_EXPORT __declspec(dllexport)
#else
#  define CCORDIS_EXPORT __attribute__((visibility("default")))
#endif
```

配套宏 `CCORDIS_PLUGIN_DEF(CLASS, NAME)` / `CCCORDIS_PLUGIN_DEF_REQ(CLASS, NAME, ...)`。

### 4.5 `ccordis/SharedLibrary.h/.cpp` — 动态库加载（唯一 OS 触点）

```cpp
class SharedLibrary {
public:
    bool load(const std::string &path);          // dlopen / LoadLibraryW
    void *resolve(const char *symbol);           // dlsym  / GetProcAddress
    bool unload();                               // dlclose / FreeLibrary
    bool isLoaded() const;
    std::string errorString() const;             // dlerror / GetLastError
private:
    void *m_handle = nullptr;                    // 头文件零 OS 头依赖
};
```

- `windows.h` / `dlfcn.h` 仅出现在 `.cpp`；Windows 侧路径按 UTF-8→宽字符转换；
- 与 Boost.DLL `shared_library` / Qt `QLibrary` 同构，可在既有这些依赖的宿主中一行替换。

### 4.6 `ccordis/Context.h` — 作用域树 + 生命周期状态机

公共 API（全部 `std::string`/`Value` 化）：

| 分组 | API |
| --- | --- |
| 装载 | `plugin(name, meta, applyFn, config)`；`plugin<T>(name, config, required)`；`plugin(name, IPlugin*, config, required)`；`pluginFromLibrary(path, config)`；`pluginFromDef(def, config)`（收养外部加载器已装载的插件，§7.6） |
| 卸载/诊断 | `unload(name)`（管理卸载：级联拆服务，库退役延迟卸载）；`pluginState(name)`；`pluginError(name)`（lastError，评审 F6） |
| 工厂/配置 | `registerFactory` / `loadRegistered` / `loadConfig(const Value&)` |
| 服务 | `provide<T>` / `inject<T>` / `watch<T>` / `watchAny` |
| 事件 | `on(event, handler)` / `emitEvent(event, payload)` |
| 数据面 | `channel(name)` / `onBlob(name, handler)` / `pumpAll()`（§4.7，全链路见 §9） |
| 中间件 | `middleware(mw, prepend)` / `run(payload)` |
| 内省 | `pluginState(name)` / `topology()` → `Value` / `registry()` / `bus()` |

### 4.7 `ccordis/Blob.h` + `ccordis/BlobChannel.h` — 数据面（大量 BLOB 快速传递）

控制面（EventBus/Value）与**数据面**（Blob 通道）分离：小而 JSON 形的控制事件走总线，MB 级二进制（频谱帧、瀑布行、IQ 块）走零拷贝通道。

**`Blob` —— 不可变字节视图 + 共享 keep-alive**：

- `Blob::allocate(n, pool)`：池化块（64B 对齐，SIMD 友好）；
- `Blob::wrap(p, n, owner)`：**零拷贝包外部内存**（设备 DMA 缓冲、宿主向量）——传 shared_ptr 作 owner 保活；
- 不可变契约：发布后写者不得再触碰字节（单写者在 publish 前完成写入）；
- 拷贝语义 = 两指针 + 一次原子计数；N 订阅者扇出 = N 次计数自增，**零字节拷贝**。

**`BlobPool` —— 回收式分配器（稳态零分配）**：

- 幂次尺寸桶（64B 起）：请求 900B 复用 1024B 桶的空闲块；
- 驻留有上界（默认 64 MiB），超预算直接还堆，防驻留膨胀；
- 线程安全（设备线程可直接取块、异地填充、再 post）；
- **共享 State 模式**：内部状态由 `shared_ptr<State>` 持有、块删除器捕获之——Blob 逃逸池生命周期后释放仍安全（State 标记 dead → 直接 plain-free），杜绝 use-after-free。

**`BlobChannel` —— 具名通道（线程契约刻意非对称）**：

| 操作 | 线程 | 语义 |
| --- | --- | --- |
| `subscribe(h)` / `publish(b)` / `pump()` | 仅宿主线程 | 同步快照扇出（与 EventBus 同快照模式） |
| `post(b)` | **任意线程**（设备线程） | 有界邮箱入队；溢出**丢最旧**（latest-wins，活流正确默认） |
| `setPumpHook(fn)` | **post 调用线程**（通常设备线程） | 空→非空转换时回调一次；只许无锁唤醒调度（Qt 侧 `QueuedConnection` invoke / eventfd），**禁止**直接触碰 QTimer/控件等宿主线程亲和对象 |

订阅 RAII（`ScopedSubscription`），`ctx.onBlob(name, h)` 作用域绑定自动退订。统计（posted/dropped/pumped/delivered，原子计数）暴露慢消费者丢帧可观测性。

**Context 集成（v2 Context 内置）**：`ctx.channel(name)`（惰建，root 持池与通道表）、`ctx.onBlob(name, h)`、`ctx.pumpAll()`。

**典型零拷贝路径**（设备→多视图）：

```
设备线程: pool.acquire → 解析填充 → channel.post(Blob)
宿主线程: pumpHook 触发 → pump() → N 个视图处理器收到同一 Blob（指针同一性）
```

**内存模型（评审 A4）**：单写者必须在 **post()/publish() 之前**完成对块的全部写入；`post()` 的邮箱互斥释放与 `pump()` 的获取构成 happens-before，消费者读到的字节必然是发布前的最终值。`wrap()` 外部内存自交接起不得再写（别名风险）。debug 构建可给池块加 canary/generation 校验（可选强化）。

**实测（本机 g++ 13.3 -O2，冒烟用例存档）**：1 MiB 帧 × 3 订阅者扇出 **1.0 µs/帧**（含池化分配）；1000 帧稳态**零堆分配**（块回收复用）；容量 4 的邮箱 10 帧突发丢最旧保最新 4 帧；跨线程 post→pump FIFO 保序；池亡后 Blob 释放无崩溃。

### 4.8 Context 实现机制（v1 已验证，v2 平移）

- `m_records: vector<unique_ptr<LoadRecord>>`——记录地址稳定，依赖哨兵 lambda 捕获安全；
- 每 Context 持 `shared_ptr<bool> m_alive` 存活令牌，watch/哨兵回调持 weak 令牌，宿主析构瞬间起静默；
- `ensureStarted`/`stopRecord` 先置状态再行动；子作用域析构引发的 revoke → 哨兵 → 兄弟插件 stop 链天然安全；
- 拆除顺序（`~Context`）：作废令牌 → 逆序停记录（级联子树 + `SharedLibrary::unload` + 摘哨兵）→ 摘服务 watch → 释放事件连接 → 逆序 revoke 服务。

---

## 5. 插件生命周期状态机

> 本章为契约级概要; 逐转移动作、拆除五阶段、退役库纪律与宿主拆除顺序的完整细化见 `plugin-lifecycle.md`。

```
                 requires 全部满足
     ┌──────────┐ ───────────────────► ┌────────┐
     │ Deferred │                      │ Active │
     │ (依赖哨兵 │ ◄────────────────── └────────┘
     │  在哨)    │    requires 中任一失效    │
     └──────────┘      (revoke→watcher)    │ ~Context / 父作用域拆除
            │ apply 抛异常                  ▼
            ▼                           ┌──────────┐
        ┌────────┐                      │ Disposed │ (终态)
        │ Failed │ (终态，§5.1)          └──────────┘
        └────────┘
```

| 转移 | 触发 | 动作 |
| --- | --- | --- |
| Deferred→Active | 哨兵观测到全部依赖上线 | 建子作用域，置 Active，调 apply |
| Active→Deferred | 任一依赖被 revoke | 拆子作用域（级联回收）→ dispose → 释放实例 → 置 Deferred 等待重启 |
| →Failed | apply 抛异常 | 同上拆除，置 Failed |
| →Disposed | 宿主作用域析构 | 同上拆除 + 卸载动态库 |

重启语义即 CORDIS 依赖活性：`A requires X`，`B provides X`——B 拆 → A 自动拆挂起；B 重供 → A 重新 apply（新实例、新子作用域），激活间资源零泄漏。

**5.1 Failed 为终态**：不自动重试（防异常风暴，CORDIS 同策略）；恢复路径为重新 `plugin()`。

---

## 6. 依赖语义细则

1. 晚绑定：装载顺序无关（先消费者后提供者等效）；
2. 依赖面向**服务 id 契约**而非插件名——测试源/设备源可互换；
3. 多依赖全部满足才启动，任一失效即拆回 Deferred；
4. 依赖环：环上全员 Deferred，`topology()` 可观测（环检测列 M4）；
5. 嵌套即子图：父拆除级联子拆除。

---

## 7. 动态插件 ABI 规范

### 7.1 符号契约

动态插件是共享库（`libcordis_<name>.so` / `cordis_<name>.dll`），导出唯一 C 符号：

```cpp
extern "C" CCORDIS_EXPORT const ccordis::PluginDef *ccordis_plugin_entry();
```

宿主 `SharedLibrary::resolve("ccordis_plugin_entry")` 取 `PluginDef*`；create/destroy 为纯函数指针，加载期无 C++ 运行时对象跨界；`IPlugin` vtable 仅在同工具链宿主/插件间穿越。

### 7.2 工具链一致性（强制）

同一编译器版本 + 同一 C++17 标准库配置（+ 若宿主为 Qt，同一 Qt 构建）。**v2 的 ABI 面比 v1 更窄**：跨边界类型只有 `const char*`、函数指针、`IPlugin` vtable、`Context&`/`Value`（标准库类型）——Qt 类型完全不出内核 ABI。CI 加动态插件往返冒烟（§13 T8）。

**布局与符号耦合（2026-09-14 动态往返实测教训）**：
- **同头文件版本**：内核内联接口（provide/on/watch 等模板）嵌入插件编译——宿主改内核头（增删成员改变布局）后**必须重编译全部插件**，否则插件会静默写错成员槽位（本次实测：陈旧 .so 令服务撤销清单错位，卸载后注册表残留 → 拆除崩溃）。CI 的 T8/T17 往返用例天然护栏（每次重打两份）。
- **宿主符号导出**：动态插件对内核符号（如 ServiceRegistry::provide）的引用需要解析来源——宿主可执行文件需 `-rdynamic` 链接导出自身符号，或将内核编译为 `libcordis.so` 供宿主与插件共同链接（正式产品推荐后者）。

**RTTI 可见性防御条款（评审 F2，2026-09-14 实证后定为防御性）**：跨 DSO 服务接口类型必须 default visibility——接口基类显式 `CCORDIS_INTERFACE`（见 `Export.h`，展开为 `__attribute__((visibility("default")))`）或以导出的 key function（如非内联虚析构）锚定唯一 typeinfo。实证：本机 g++ 13.3/libstdc++ 下 hidden 双份 `type_info` 时 `operator==` 仍因 strcmp 回退判定 EQUAL（安全）；libc++（macOS 同源）等实现无完整回退，故保留强制条款 + T17 双 .so 注入往返用例。

### 7.3 卸载时机

`SharedLibrary::unload()` 只在宿主作用域析构时执行（`~Context`），依赖重启只销毁实例（`def->destroy()`），库保持驻留——杜绝悬垂 vtable/静态析构竞态。**管理卸载（`Context::unload`）同理**：只杀服务与实例，动态库转入宿主持有的"退役列表"，待宿主作用域析构才真正卸载（实测教训：立即卸载会让仍被消费者 shared_ptr 持有的服务对象分派到未映射内存）。

### 7.4 销毁纪律

宿主永不 `delete` 插件实例：对象形态所有权在调用方（仅调 dispose），动态形态必须经 `def->destroy()`；类形态实例由 `LoadRecord::owned`（`shared_ptr<void>` + 类型化删除器）持有。

### 7.5 最小动态插件（纯 C++17，无 Qt）

```cpp
// plugins/hello/hello.cpp → libccordis_hello.so
#include "ccordis/Plugin.h"
class HelloPlugin : public ccordis::IPlugin {
    void apply(ccordis::Context &ctx, const ccordis::Value &) override {
        ctx.on("app.ready", [](const ccordis::Value &) {
            // 纯 std 实现的插件体
        });
    }
};
CCORDIS_PLUGIN_DEF(HelloPlugin, "demo.hello")
```

### 7.6 与 QLibrary（或其他加载器）的互操作

`SharedLibrary` 与 `QLibrary`/`Boost.DLL` 同居一进程时，坐在**同一组 OS 原语**上（dlopen/dlsym/dlclose ↔ LoadLibrary/GetProcAddress/FreeLibrary），互操作由 OS 引用计数天然保证，**无需桥接层**。本机实测（Qt 5.15.13 + glibc，探针存档于调研记录）：

| 现象 | 实测结果 |
| --- | --- |
| QLibrary 默认 flags | **RTLD_LOCAL**（符号不进全局命名空间，`dlsym(RTLD_DEFAULT)` 查不到） |
| 同一文件被 QLibrary 与裸 dlopen 各加载一次 | 同一 OS 对象，引用计数 +1，不重复映射 |
| 我方 dlclose 后 QLibrary 侧 resolve | 仍可用（计数未归零） |
| QLibrary::unload() | 计数归零时**真实 dlclose** |

互操作三模式（按推荐顺序）：

1. **服务级通讯（首选）**：一切跨插件通讯走 Context 的 provide/inject/watch 与 EventBus。Qt 侧插件经 `adapters/qt` 垫片装进 cordis（host 用 QPluginLoader/QLibrary 加载 → 把其接口 `provide<T>` 成服务）。与加载器无关，与语言绑定无关。
2. **C-ABI 收养**：`Context::pluginFromDef(const PluginDef *def, const Value &config)`（v2 新增 API）——宿主用**任何**加载器（QLibrary/Boost.DLL/手工 dlopen）解析出 `ccordis_plugin_entry` 后，把 `PluginDef*` 交给内核收养；内核调用 create/apply/destroy 但**永不卸载该库**（卸载权留在原加载器）。反向亦然：宿主可用 QLibrary 去 resolve 内核加载的库（同一 OS 对象）。
3. **普通链接依赖**：cordis 插件若要用 Qt 类型，直接正常链接 Qt（DT_NEEDED），ELF 依赖解析与加载器无关——"内核不依赖 Qt"不禁止"某个插件依赖 Qt"。

互操作纪律（违反即 UB 风险）：

- **一文件一卸载权**：cordis `pluginFromLibrary` 拥有的库只由内核在宿主析构时卸载；收养（pluginFromDef）的库内核永不卸载；宿主不得对内核拥有的路径调用 `QLibrary::unload()`；
- **路径规范化**：跨加载器命中同一 OS 对象的前提是路径一致——内核统一 `std::filesystem::weakly_canonical`，宿主侧同样传绝对规范路径；
- **flags 对齐**：内核默认 `RTLD_LAZY|RTLD_LOCAL`（与 QLibrary 默认一致，实测）；**禁用 RTLD_GLOBAL**——跨插件符号互查一律走内核 resolve 或服务总线；不依赖 glibc "二次 dlopen 可升级 GLOBAL" 的未可移植行为；
- **符号可见性**：插件 `-fvisibility=hidden` + 仅导出 `ccordis_plugin_entry`（Windows 侧 CCORDIS_EXPORT）；
- **静态初始化**：dlopen 在加载线程跑插件静态构造——保持廉价，禁止回调内核；
- **错误串即时捕获**：dlerror 线程局部且易被清空，失败后立即存入 errorString（Windows GetLastError 同理）。

---

## 8. 配置驱动装载

内核接受 `Value` 形配置文档（解析属宿主）：

```cpp
ccordis::Value doc = ccordis::Value::object({
    {"plugins", ccordis::Value::array({
        ccordis::Value::object({
            {"plugin",  "source.simulator"},
            {"options", ccordis::Value::object({{"rateHz", 5}})}}),
        ccordis::Value::object({
            {"plugin",  "lib:plugins/libccordis_hello.so"}}),
    })}});
ctx.loadConfig(doc);
```

- 工厂名（进程内 `registerFactory`）与 `"lib:<路径>"`（SharedLibrary）统一同表；
- 未知名字经 logger 告警并跳过，不致命；
- options 原样透传，schema 由插件自定义。

---

## 9. 数据传输与服务流程设计

数据传输采用**双平面模型**，方向对称、语义不同：

| 平面 | 载体 | 方向 | 语义 | 可靠性 |
| --- | --- | --- | --- | --- |
| 控制面（命令/事件） | EventBus + `Value` | 双向 | 小、结构化、低频（用户操作、ack、状态变更） | 进程内可靠（同步分发不丢） |
| 数据面（帧流） | BlobChannel + `Blob` | 设备→界面为主 | 大、二进制、高频（频谱帧、瀑布行） | 尽力而为，溢出丢最旧（latest-wins） |

两个平面共享同一条线程纪律（§12）：**跨界只许两跳**——IO 线程产出、宿主线程消费，中间由 `post()/pump()` 或 `QueuedConnection` 桥接。

### 9.1 上行数据流：设备 → 界面（帧流）

```
[IO/设备线程]                          [宿主(GUI)线程]
pool.acquire(n)
  解析协议帧 → 填充池块
  Blob::wrap(data, n, keepAlive)
  channel("spectrum.frames").post(blob) ──空→非空──► pumpHook 触发
                                              QTimer(0) → pump()
                                                 ├─► 视图A订阅者 ─┐
                                                 ├─► 视图B订阅者 ─┤ 同一 Blob（指针同一，零拷贝）
                                                 └─► 视图C订阅者 ─┘
```

要点：解析在 IO 线程完成（POD 化，符合 VpxDeviceClient 既有军规）；池块在 IO 线程填充，post 后字节冻结（不可变契约）；慢视图不拖快视图——邮箱丢最旧只影响自己消费到哪帧，通道 stats 可观测。

### 9.2 下行命令流：界面操作 → 网络协议 → 服务（本节回答"用户操作如何到服务"）

**分层与职责**：

| 层 | 职责 | 线程 |
| --- | --- | --- |
| 视图层 | Qt 槽函数捕获用户操作，调用注入的命令服务，**立即返回** | GUI |
| 命令服务层（`device.command` 服务，由 `conn.vpxclient` 插件提供） | 参数校验、去抖合并、requestId 分配、超时管理 | GUI（登记）/IO（执行） |
| 协议编码层（vpxproto adapter） | POD 命令 → 协议帧字节 | IO |
| IO 层（VpxDeviceClient/asio TaskLoop） | strand 串行化、UDP/TCP 发送、收包解析 | IO |
| 宿主/中间件/服务 | scouter 转发到设备板卡 | — |

**时序（下行命令 + 回程 ack/数据）**：

```
[GUI线程]                            [IO线程(TaskLoop/asio)]      [scouter]  [设备]
Qt slot: 用户拖动频段
  │ cmd = ctx.inject<DeviceCommandService>("device.command")
  │ id = cmd->setBand(30, 6000)   ──立即返回(非阻塞)──┐
  │      └─ POD命令 strand.post ────────────────────►│ vpxproto 编码
  │                                                  │ UDP send ──────────►│──►│
  │                                                  │                     │  │
  │                                                  │◄── 响应/通知帧 ─────┤◄──┘
  │                                                  │ 解析为 POD (mutex)
  │◄──── Qt::QueuedConnection ───────────────────────┤
  │ emitEvent("cmd.ack", {{"id",id},{"code",0}})      │
  │ [若是数据帧] channel.post(Blob) → 同 §9.1 上行      │
  │ [超时] 服务层定时器 → emitEvent("cmd.timeout",{id})│
```

**实现契约（五条军规）**：

1. **UI 永不阻塞**：命令服务方法全部立即返回 `requestId`；成败经 `cmd.ack / cmd.nak / cmd.timeout` 事件回报，视图按需订阅回显（如 toast/状态栏）。
2. **编码与发送只发生在 IO 线程**：GUI 线程只做"打包 POD 命令 + post 到 strand"，协议编码、socket 读写、收包解析全在 TaskLoop——与既有 VpxDeviceClient 军规一致，杜绝 GUI 线程触碰网络与跨线程 QObject。
3. **requestId 关联**：命令发出时登记 `id → (类型, 超时截止)`；IO 线程回包经 QueuedConnection 跳回 GUI 线程后匹配销账；超时表由服务层每秒扫一次（或用宿主定时器）。
4. **去抖合并**：连续 UI 事件（拖动滑杆产生的一串 setBand）在服务层合并为"尾值命令"（50–100ms 窗口），只在窗口关闭时真正下发——设备带宽友好。
5. **类型化接口为主，命令封套为辅**：编译期已知命令用类型化服务接口（强类型、可发现误用）；动态插件扩展命令走 `Value` 封套事件 `device.cmd`（见 §9.3），由连接插件统一翻译成协议帧。

**代码骨架（类型化主路径）**：

```cpp
// ── 命令服务契约（可放公共头，纯 C++17）──────────────────────
class DeviceCommandService {
public:
    virtual ~DeviceCommandService() = default;
    virtual std::uint64_t setBand(double minMHz, double maxMHz) = 0;  // 立即返回
    virtual std::uint64_t startScan() = 0;
    virtual std::uint64_t queryCali() = 0;
};

// ── conn.vpxclient 插件（类形态）装载该服务 ──────────────────
class VpxClientPlugin {
public:
    VpxClientPlugin(ccordis::Context &ctx, const ccordis::Value &cfg)
        : m_client(std::make_shared<VpxDeviceClient>()) {
        m_client->connect(cfg.at("host").toString("127.0.0.1"),
                          cfg.at("port").toInt(10097));
        ctx.provide<DeviceCommandService>("device.command",
                                          std::make_shared<CmdFacade>(m_client, ctx));
        ctx.on("device.cmd", [this](const ccordis::Value &env) {     // 封套路径
            m_client->postEnvelope(env);
        });
    }
private:
    std::shared_ptr<VpxDeviceClient> m_client;
};
ctx.plugin<VpxClientPlugin>("conn.vpxclient", cfg, /*requires*/ {});

// ── 视图/主窗口侧（Qt 桥: signal → 服务调用）──────────────────
void MainWindow::onBandChanged(double min, double max) {           // GUI 线程
    if (auto cmd = m_ctx.inject<DeviceCommandService>("device.command"))
        m_pendingBandCmd = cmd->setBand(min, max);                  // 非阻塞
}
```

#### 请求超时处理（`ccordis/RequestTracker` + 分层策略）

超时机制分三层，各归其位：

**第 1 层：跟踪器（内核工具，纯数据结构，已实现）**——`ccordis/RequestTracker`：

- 不含 OS 定时器、不开线程、不回调用户代码——时间由宿主经 `tick(now)` 注入（Qt 宿主用一个 100ms `QTimer`；纯 C++ 宿主用主循环节拍），`track()` 与 `tick()` 共享同一可注入时钟，确定且可测；
- 请求状态机：`track(id, timeout)` 登记 → `complete(id)` 归类（**Acked**＝在途收到响应 / **LateAck**＝超时后才到（256 条有界历史内单次分类）/ **Unknown**＝从未登记或已被 fail-fast 清空）；`tick(now)` 按到期顺序返回过期 id；`forget(id)` 供重试换新 id 时销旧账（不计入 acked/timedOut，统计反映结果而非尝试次数）；同 id 重复 track 为期限刷新；
- `stats()`（inFlight/acked/timedOut/lateAcks）暴露可观测性。

**第 2 层：策略（命令服务 facade，宿主侧）**——把 `tick()` 的过期 id 翻译成动作：

| 要素 | 规则 |
| --- | --- |
| 超时表（按命令类型） | SET_BAND 500ms / SCAN 控制 2s / 校准查询 5s（可配置） |
| 重试 | **仅幂等命令**可重试：`forget(旧id)` → 新 requestId 重发 → 退避 200ms→600ms，上限 2 次；非幂等命令直接失败 |
| 终态上报 | 重试耗尽 `emitEvent("cmd.timeout", {{"opId",..},{"attempts",..}})`；成功 `cmd.ack` |
| **fail-fast** | 心跳丢失/连接断开 → `expireAll()` 一次清空全部在途（按各自到期顺序返回），**不等逐条超时**；迟到响应此后归类 Unknown，静默忽略 |
| 迟到响应 | `LateAck` → `emitEvent("cmd.late", {id})` 仅供日志/状态对账，**不改 UI 状态**；真实状态由周期查询或下一帧数据对账 |

**第 3 层：UX（视图层）**——乐观 UI + pending 态：操作即时生效并挂起指示；`cmd.ack` 落定；`cmd.timeout` 回滚乐观状态或弹"重试"；pending 期间按钮去抖（防重复提交叠加在途请求）。

实测（冒烟用例全绿）：到期按期限顺序触发、Acked/LateAck/Unknown 三分类正确、二次迟到不再误分类、fail-fast 清空、期限刷新语义、统计一致。

### 9.3 帧封装与命令封套

**数据面 Blob 帧头**（32 字节固定头 + SIMD 对齐后紧跟 payload）：

```
偏移  长度  字段           说明
0     4    magic          帧标识 'BLOB'（0x42 0x4C 0x4F 0x42）
4     1    version        帧格式版本（当前 = 1）
5     1    type           帧类型（见下表）
6     1    subType        子类型（type 大类下细分）
7     1    reserved       对齐填充
8     4    seq            帧序号（丢帧检测：seq 跳变 = 丢了 N 帧）
12    4    payloadLen     载荷长度（不含 32 字节头）
16    8    timestampNs    采样时间戳（单调钟，ns）
24    8    ext            扩展区（按 type 解释，见下表）
```

**type / subType 枚举与扩展区 ext（offset 24, 8 字节）解释**：

| type 值 | 含义 | subType | ext[0:4] | ext[4:8] |
| --- | --- | --- | --- | --- |
| 0x01 | 频谱数据 | 0=实时 1=平均 2=最大保持 3=最小保持 | startFreq（起始频率，MHz，float32 大端） | segment（分段号，uint8）+ 3B reserved |
| 0x02 | 门限数据 | 0=门限线 1=余量 | startFreq（起始频率，MHz，float32 大端） | segment（分段号，uint8）+ 3B reserved |
| 0x03 | OCC 数据 | 0=占用度 1=余量 | startFreq（起始频率，MHz，float32 大端） | segment（分段号，uint8）+ 3B reserved |
| 0x04 | IQ 数据 | 0=原始 1=解调后 | centerFreq（中心频率，MHz，float32 大端） | sampleRate（采样率，Hz，float32 大端） |
| 0x05 | 测向预处理 | 0=常规 1=空间谱 | startFreq（起始频率，MHz，float32 大端） | 4B reserved |
| 0x06 | 信号检测 | 0=检测 1=合并 | startFreq（起始频率，MHz，float32 大端） | 4B reserved |
| 0x07 | 瀑布行 | 0=FFT 1=平滑 | startFreq（起始频率，MHz，float32 大端） | segment（分段号，uint8）+ 3B reserved |
| 0x0F | 用户扩展 | 自定义 | 自定义 | 自定义 |

> 注：频率字段统一 **float32 大端**（网络序），payload 内功率/幅度单位由 type 隐含（频谱=dBm，OCC=0/1 或百分比，IQ=归一化 I/Q）。扩展区未用字节填 0；新增 type 不破坏旧解析（旧节点按 reserved 跳过未知 type）。

**命令封套（控制面 `Value` schema，封套路径用）**：

```cpp
ccordis::Value::object({
    {"type",   "SET_BAND"},          // 命令名（连接插件翻译到协议码）
    {"id",     nextRequestId()},     // 调用方侧关联
    {"params", ccordis::Value::object({{"min", 30.0}, {"max", 6000.0}})},
})
```

### 9.4 背压与可靠性分层

| 流量 | 策略 | 载体 |
| --- | --- | --- |
| 帧流（高频数据） | 丢最旧保最新；seq 跳变可检测丢帧量 | BlobChannel 邮箱 + 帧头 seq |
| 命令（低频控制） | requestId + ack/nak + 超时；去抖合并 | EventBus 事件 + 服务层登记表 |
| 状态事件 | 进程内同步分发，天然可靠 | EventBus |

### 9.5 与本仓设备拓扑的映射

```
demo (UDP 本机 10197, 固定) ◄──► scouter (localhost:10097) ◄──► 设备 (192.168.10.120:10091)
```

- 上行：`0x9201` 频谱 / 迭代频段 `0xb441/0xb442` / 阶段结束 `0xb444` → IO 线程解析 → `spectrum.frames` / `iter.bands` 通道；
- 下行：频段设置、校准查询（`RECV_CALI_QUERY 0x95C0`）、扫描控制 → `DeviceCommandService` → vpxproto 编码 → scouter → 设备；
- 线程军规即 AGENTS.md 既有规约：**禁止从 TaskLoop 触碰 QObject/Context**，回程必须 QueuedConnection。

---

## 10. 宿主适配层（Qt 桥，M2 交付）

`adapters/qt/`（宿主侧代码，**不属于内核**，不受 §0 约束）：

| 适配点 | 实现 |
| --- | --- |
| 字符串 | `QString::fromStdString` / `toStdString`（UTF-8 往返） |
| 配置互转 | `QJsonObject` ↔ `ccordis::Value`（递归转换，~60 行） |
| 文件装载 | 读 JSON → QJson → Value → `loadConfig` |
| GUI 服务 | `provide<QWidget>`（见 §4.3）；Qt 事件 → `emitEvent` 的桥 |
| 线程 | 设备线程 → `QueuedConnection` 跳 GUI 线程后才能碰 Context |

---

## 11. 与 SpectrumExplorerDemo 集成方案

### 11.1 目标划分：核心服务 vs 插件

核心服务（宿主直接注册）：`model.core`（SignalModel）、`scheduler.newtarget`（NewTargetScheduler）、`theme.manager`（ThemeManager）。

插件：`source.simulator` / `source.vpx`（provides `signal.source`/`spectrum.source`，requires `model.core`/`vpx.client`）、`conn.vpxclient`、`view.spectrum`/`view.waterfall`/`view.list`/`view.statusbar`（经 `provide<QWidget>` 挂入）、`nav.*` 微插件（对接 P1 四平面）。

MainWindow 保持宿主编排者身份（选择集/时间窗/四级提示门控）。

### 11.2 迁移路径

1. **M2**：app `.pro` 升 `CONFIG += c++17`；`main()` 建 root Context，注册核心服务；SignalSimulator 改函数形态插件，默认行为不变；
2. **M3a**：vpx 后端 + 连接管理插件化（`--auto-connect`/环境变量语义不变）；
3. **M3b**：四视图插件化（`adapters/qt` 的 QWidget 服务模式）；
4. **M3c**：`plugins.json` 外置配置 + topology 导出 graphviz；
5. 每步全量 `tst_spectrum` 回归 + offscreen 冒烟。

### 11.3 与 P1 四平面的衔接

`nav/` 已具备数据/映射/图层/效果四平面接口，M3 阶段把注册入口改走 `ctx`（图层/效果作为微插件 provides `nav.layer.*`/`nav.fx.*`），不改四平面本体。

### 11.4 专利载荷保护条款（红线）

NewTargetScheduler 的 Δf_min = k·min(B_i,B_j)、P_i = C_i·π_i、ρ=N/B 及 L1–L4 提示时序（含 L3 +0.5s）等常数/公式是权利要求载荷；插件化只允许改调用关系，不允许改计算与时序本体；涉改动必须同步 `PATENT_APPLICATION_CN_v10.md` 与 `PATENT_MAPPING.md`。

---

## 12. 线程模型

内核默认单线程（宿主线程）使用；Registry/EventBus 内部互斥仅为拆除重入与快照一致性，**不构成**跨线程发布许可。**唯一例外是数据面 `BlobChannel::post()`**（§9）：IO/设备线程允许 post；订阅、publish、pump 仍严格限宿主线程。命令流回程沿既有规约（asio 回调 → POD → `QueuedConnection` → GUI 线程）后才能触碰 Context——设备线程永不直接调用 EventBus/Registry/Context 方法。

---

## 13. 测试方案（`tests/tst_Ccordis.cpp`，Qt Test 仅作 runner，用例只碰 std 类型）

> 实施状态（2026-09-14）：T1–T17 语义已由三份独立冒烟可执行验证全绿（内核 12 场景含动态 .so 往返与跨 DSO 注入、数据面、超时跟踪），并过 ASAN/UBSAN；Qt Test 化接入 `tests.pro` 为 M0 收尾事项。

| # | 用例 | 断言要点 |
| --- | --- | --- |
| T0 | **内核纯净性** | CI 在**无 Qt include 路径**下 `g++ -std=c++17 -fsyntax-only` 编译包含全部内核头的 TU |
| T1 | 事件总线 | on/emitEvent 触发；ScopedConnection 析构退订；emitEvent 中退订不崩（快照） |
| T2 | 服务类型严格性 | 错型 inject → nullptr |
| T3 | 晚绑定 watch | 先 watch 后 provide 触发；revoke 触发 nullptr |
| T4 | 嵌套级联 | 子作用域析构撤销其 provide/on/watch，兄弟不受扰 |
| T5 | 依赖状态机 | 缺失→Deferred；上线→Active；下线→Deferred；再上线重启（重启计数） |
| T6 | 异常→Failed | apply 抛异常被捕获，无泄漏扩散 |
| T7 | 中间件 | append/prepend 顺序；不调 next 短路 |
| T8 | 动态库往返 | SharedLibrary 加载 libccordis_hello.so → 服务出现 → 宿主析构干净（缺库 QSKIP） |
| T9 | loadConfig | 工厂名 + lib: 前缀 + 未知名跳过 |
| T10 | topology | Value 树结构/状态/children/provides |
| T11 | Value 类型 | variant 构造/取值/缺键 null/数组对象嵌套 |
| T12 | **数据面零拷贝** | 扇出后 N 订阅者 `data()` 指针同一；池分配计数=1 |
| T13 | **池回收** | 同桶块释放后复用命中；64B 对齐；稳态零分配（增量断言） |
| T14 | **邮箱与跨线程** | 溢出丢最旧；post(任意线程)→pump FIFO 保序；hook 单次触发且**在 post 调用线程执行**（A1 契约） |
| T15 | **池亡安全** | Blob 逃逸池生命周期后释放无崩溃（共享 State 模式） |
| T16 | **请求超时** | 到期顺序触发；Acked/LateAck/Unknown 三分类；fail-fast 清空；期限刷新；统计一致 |
| T17 | **跨 DSO 注入往返** | 宿主与插件各持接口头编译，双实体 provide/inject 等价（RTTI 可见性护栏） |

---

## 14. 目录结构与实现状态

```
ccordis/   （v2 全量实现；纯 C++17 零 Qt，8 TU 纯净性零警告，
           内核 12 场景冒烟 + 数据面/超时冒烟 + ASAN/UBSAN 全绿）
  Export.h                符号可见性宏 + CCORDIS_INTERFACE          [✓]
  Value.h                 std::variant 值类型(header-only,保序Object) [✓]
  Log.h/.cpp              可注入日志(默认stderr)                   [✓]
  EventBus.h/.cpp         事件总线(FIFO承诺+异常隔离)              [✓]
  ServiceRegistry.h/.cpp  类型化注册表 + 观察者                    [✓]
  Plugin.h                IPlugin/PluginMeta/PluginDef + 宏        [✓]
  SharedLibrary.h/.cpp    dlopen/LoadLibrary 封装(唯一OS触点)      [✓]
  Context.h/.cpp          作用域树+状态机+unload+退役库+通道集成    [✓]
  Blob.h/.cpp             数据面: 不可变视图 + 回收池(共享State)   [✓]
  BlobChannel.h/.cpp      数据面: 具名通道(扇出/邮箱/pump)         [✓]
  RequestTracker.h/.cpp   命令面: tick驱动请求超时跟踪器(§9.2)     [✓]
adapters/qt/              Qt 桥(宿主侧)                          [M2]
plugins/hello/            纯 C++17 动态插件示例                    [M1]
tests/tst_Ccordis.cpp      §13 用例                                [M0]
```

v1 内核（Qt 地基）已通过 g++11/17 + clang 双标准编译验证；v2 重构为机械平移（类型替换 + SharedLibrary/Export/Value 三个新部件），状态机与拆除逻辑不变。

---

## 15. 风险与对策

| 风险 | 等级 | 对策 |
| --- | --- | --- |
| 动态插件 ABI 依赖同工具链 | 中 | §7.2 强制 + CI T8 冒烟；v2 ABI 面已收窄到纯 std |
| app 升 C++17 的存量破坏（register/throw() 等） | 中 | 升级前全量编译；vpxproto 预编译库 ABI 无关，头文件已实测可过 |
| Failed 终态"看起来死了" | 低 | topology 可观测 + logger；重装载即恢复 |
| 依赖环静默挂起 | 中 | topology 可观测；M4 环检测 |
| 注入指针越过提供方生命周期 | 低 | shared_ptr 内存安全；语义"失维护" |
| SharedLibrary 卸载悬垂 | 中 | §7.3 仅宿主析构卸载 |
| 慢消费者导致数据面丢帧 | 低 | 邮箱丢最旧是活流正确语义；stats 原子计数可观测，容量可调 |
| 池驻留内存膨胀 | 低 | maxRetained 上界（默认 64 MiB），超预算直接还堆 |
| 专利载荷误伤 | 高 | §11.4 红线 + PATENT_MAPPING 对齐流程 |

---

## 16. 里程碑

| 阶段 | 内容 | 验收 |
| --- | --- | --- |
| M0 | v2 内核重构（去 Qt）+ Value/SharedLibrary/Export + tst_Ccordis 全绿 + **T0 纯净性** | 无 Qt 路径编译通过；全用例绿；旧用例零回归 |
| M1 | 纯 C++17 动态插件示例 + `.pro` 接线（app 升 c++17） | T8 往返；主程序 offscreen 冒烟 |
| M2 | `adapters/qt` 桥 + 宿主接入（root Context + 核心服务 + source.simulator 插件化） | 默认 mock 链路行为不变 |
| M3 | 视图/设备后端插件化 + plugins.json + topology 可视化；**命令链路（§9.2）落地**：DeviceCommandService + ack/timeout + 去抖 | 全量回归 + 关系图产出；命令 mock 往返（UI 调用→ack 事件→状态栏回显）|
| M4 | 环检测、热卸载评估、B 方案重估 | 另立设计 |

---

## 17. 许可证合规

`ccordis/` 为本仓原创（CORDIS 理论的 C++ 再表达）；未链接/复制 CppMicroServices 代码（Apache-2.0 文档语义参照）；Qt 仅存在于宿主与适配层，沿用项目既有 LGPL 动态链接合规方式；CORDIS（MIT）出处已在文档注明。

---

## 附录 A：CppMicroServices API 语义对照

| CppMicroServices | cordis v2 |
| --- | --- |
| `Framework` / `FrameworkFactory` | root `Context` |
| `BundleContext` | 子 `Context` |
| `BundleActivator::Load/Unload` | `apply` / `dispose`（+RAII 拆除） |
| `US_EXPORT_BUNDLE_ACTIVATOR` | `CCORDIS_PLUGIN_DEF(_REQ)` + `Export.h` |
| `RegisterService<T>` / `GetServiceReference<T>` | `provide<T>` / `inject<T>` |
| `ServiceTracker<T>` | `watch<T>` |
| Bundle 状态 INSTALLED/RESOLVED/ACTIVE | Deferred/Active（+Failed/Disposed） |
| `usResourceCompiler` 清单 | `PluginMeta`/`PluginDef` + `Value` 配置 |
| Bundle 版本/依赖协商 | 不做（非目标） |

## 附录 B：三形态速查（纯 C++17）

```cpp
ccordis::Context ctx;                                          // 根作用域

// ── 函数形态 ─────────────────────────────────────────────
ctx.plugin("source.simulator",
           ccordis::PluginMeta{"source.simulator", "sim", {}, {"signal.source"}},
           [](ccordis::Context &c, const ccordis::Value &cfg) {
               auto rate = cfg.at("rateHz").toInt(5);
               auto sim = std::make_shared<SimBackend>(rate);
               c.provide<IBackend>("signal.source", sim);
               c.on("band.changed", [sim](const ccordis::Value &p) {
                   sim->setBand(p.at("min").toDouble(), p.at("max").toDouble());
               });
           },
           ccordis::Value::object({{"rateHz", 5}}));

// ── 类形态：构造函数(Context&, const Value&) 即插件体 ──────
class WaterfallPlugin {
public:
    WaterfallPlugin(ccordis::Context &ctx, const ccordis::Value &) { /* ... */ }
};
ctx.plugin<WaterfallPlugin>("view.waterfall", ccordis::Value(),
                            {"model.core"});

// ── 对象形态：预构造实例，所有权在调用方 ───────────────────
HelloPlugin hello;
ctx.plugin("demo.hello", &hello);

// ── 动态 / 配置驱动 ───────────────────────────────────────
ctx.pluginFromLibrary("plugins/libccordis_hello.so");
ctx.loadConfig(ccordis::Value::object({
    {"plugins", ccordis::Value::array({
        ccordis::Value::object({{"plugin", "demo.hello"}})})}}));
```
