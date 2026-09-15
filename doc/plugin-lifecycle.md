# ccordis 插件生命周期详解

> 状态: v1 (2026-09-15, 随 ccordis 独立化编写)
> 对象: 内核 2.2.0 / abi 2。本文细化设计方案 §5 的紧凑状态机, 逐条对应
> `include/ccordis/Context.h` 与 `src/Context.cpp` 的实际实现; 两者冲突时以代码为准。
> 前置阅读: `ccordis-plugin-system-design.md` §4.6/§5(契约背景)、`ccordis-plugin-guide.md`(插件作者视角)。

---

## 1. 状态机总览

```
                plugin() / pluginFromLibrary() / loadAppConfig()
                          │  addRecord: 建档 → 布防依赖哨兵 → log "loaded"
                          ▼
                    ┌───────────┐   requires 全部满足 (ensureStarted)
        ┌──────────►│ Deferred  │ ────────────────────────────► ┌────────┐
        │           │ (哨兵在哨) │                               │ Active │
        │           └───────────┘ ◄──────────────────────────── └────────┘
        │             ▲              任一 require 被 revoke            │
        │             │             (哨兵观测 null → stopRecord)
        │             │                                               │
        │             │            apply 抛异常 (stopRecord)           │
        │             │              ┌──────────┐                     │
        │             └──────────────│          │                     │
        │               Failed 为终态 │ Failed   │                     │
        │               不自动重试    └──────────┘                     │
        │                                                      主动 unload()
        │            依赖重启(可再激活)◄──┐                        │
        └─────────────────────────────────┘                        ▼
                    ┌──────────┐        ~Context(宿主作用域析构)  ┌──────────┐
                    │ Disposed │ ◄────────────────────────────── │ (卸库)   │
                    └──────────┘            终态                  └──────────┘
```

| 状态 | 含义 | 触发离开 |
| --- | --- | --- |
| `Deferred` | 已建档未激活: 依赖哨兵(`depWatchTokens`)在哨, 等待全部 required 服务上线 | 全部满足 → `Active`; 作用域拆除 → `Disposed` |
| `Active` | 子作用域存活, `apply` 已执行(或正在执行) | 依赖失效 → `Deferred`; 主动卸载/拆除 → `Disposed`; (仅在 `apply` 内) 异常 → `Failed` |
| `Failed` | **终态**: 最近一次激活抛异常, `pluginError()` 留有诊断 | 仅重新 `plugin()` 装载可恢复 |
| `Disposed` | **终态**: 记录已拆除(作用域析构或管理卸载遗忘) | — |

注意 `pluginState(name)` 对**未知名**返回 `Disposed`(等价"未装载于本作用域"), 不报错——内省 API 全部零异常。

---

## 2. 装载入口与形态所有权矩阵

同一状态机覆盖全部装载入口; 差别只在**实例的创建/销毁/库所有权**:

| 形态 | 入口 | 实例创建时机 | 实例销毁 | dispose() | 库(.so)所有权 |
| --- | --- | --- | --- | --- | --- |
| 函数形态 | `plugin(name, meta, applyFn, cfg)` | 无实例(闭包即插件体) | — | — | — |
| 类形态 | `plugin<T>(name, cfg, required, ver)` | **每次激活**前 `new T(scope, cfg)`, 挂入 `record->owned` | `stopRecord` 中 `owned.clear()`(类型化删除器) | — | — |
| 对象形态 | `plugin(name, IPlugin*, cfg, required)` | 调用方预建 | **调用方持有**, 内核不删 | ✓ 每次 stop | — |
| 动态形态 | `pluginFromLibrary(path)` / `PluginRegistry::load` / `loadAppConfig` 的 `lib:` 前缀 | **每次激活** `def->create()` | **每次 stop** `def->destroy()` | ✓ | 宿主作用域独占; 卸载仅发生在 `~Context` |
| 外来收养 | `pluginFromDef(def)` (QLibrary/dlopen 互操作, 设计 §7.6) | 同动态形态 | 同动态形态 | ✓ | **外部加载器**, 内核永不卸载 |

装载链路(addRecord, 顺序即日志顺序):

1. `LoadRecord` 建档(地址稳定, `unique_ptr` 池);
2. 记录 `lastLoadedName/Version`(内省用, 勿在其上建逻辑);
3. `armDependencyWatchers`: required 非空则逐依赖布防服务哨兵;
4. 日志 `plugin loaded: 'name' vX.Y.Z (kernel …, abi …)`——**装载与激活是两个独立事件**, 分别记账(设计 §4.5.4);
5. `ensureStarted`: 依赖已满足则立即激活, 否则日志 `plugin deferred: … (requirements not yet met)`。

动态形态装载前置检查(任一失败→返回 false 并留日志, **不建档**):

- `SharedLibrary::load(path)` 成功;
- 导出 `ccordis_plugin_entry()` 且可解析;
- **G1 ABI 握手**: `def->abiVersion == CCORDIS_ABI_VERSION`, 不等则拒载并提示"用当前内核头重编插件"。这一步必须发生在读取 `def->version` **之前**(abi 1 的陈旧描述符没有尾部 version 字段, 先读即越界)。

---

## 3. 激活: `ensureStarted(Deferred → Active)`

```
守卫: state==Deferred && apply 有效; 否则直接返回(幂等)
  │
  ├─ requiresMet(required)? 逐项 registry().available(r); 任一缺席 → 仍 Deferred, 返回
  │
  ├─ state = Active            ← 先置态: 对哨兵回调的重入守卫(apply 内部再触发
  │                              依赖变化时, 状态机已一致)
  ├─ scope = new Context(this) ← 每次激活一个全新子作用域(共享 root 的
  │                              registry/bus/channels, 独立的服务撤销清单与订阅)
  ├─ m_startingRecord = &rec   ← 类形态嵌套绑定: apply 内再调 plugin<T>() 时,
  │                              实例经 currentRecord() 挂进"正在启动"的记录
  ├─ try { apply(*scope, config) }
  │    catch (exception/...) → lastError 记诊断 + 日志 threw
  │                            → stopRecord(rec, Failed)   ← 子作用域一并拆除
  └─ state==Active → 日志 plugin activated(每次激活都记, 含依赖重启的再激活)
```

要点:

- **晚绑定**: 装载顺序无关。消费者先装载即 Deferred, 提供者上线瞬间由哨兵触发激活。
- **apply 的语义**是"在本插件专属作用域内登记资源"(provide/on/watch/onBlob/plugin 子插件), 不是长驻线程; 内核不提供独立线程模型(§10)。
- **异常只在 apply 期间**产生 Failed; dispose/拆除路径的异常由内核捕获并降级为日志(见 §4), 不改变状态机。
- `pluginFromLibrary` 的 apply 闭包在**每次激活**重新 `def->create()`——依赖重启后的动态插件拿到的是新实例, 与类形态语义一致。

---

## 4. 拆除: `stopRecord(Active/Failed → Deferred|Failed|Disposed)`

```
守卫: state==Active || Failed(Deferred/Disposed 幂等返回)
  │
  ├─ state = target            ← 先置态: 重入守卫(拆除级联可能再进本函数)
  │
  ├─ 1. scope.reset()          ← 子作用域析构, 五阶段级联(§5)。该插件提供的服务
  │        在此被撤销 → 下游哨兵观测 null → 下游 stopRecord(Deferred),
  │        依赖变化沿服务图涟漪传播
  ├─ 2. dispose()              ← 对象/动态形态(callDispose); 内核 catch 全部异常
  │        并记日志 "dispose threw"——dispose 抛异常按 bug 对待, 不阻断后续步骤
  ├─ 3. def->destroy(instance) ← 动态形态: 插件侧配对删除(宿主永不 delete)
  ├─ 4. objectInstance = null  ← 对象形态: 仅摘引用, 实例归调用方
  └─ 5. owned.clear()          ← 类形态实例析构(类型化删除器, LIFO)
```

顺序语义(刻意设计): **子作用域先于 dispose 拆除**——dispose() 执行时, 本插件提供的服务、作用域订阅已全部撤销, dispose 是插件对"我的登记已清空"的最后确认, 而非清理入口。若需要在服务仍可见时收尾, 应在 apply 登记的资源自身析构里做(RAII 先于 dispose)。

---

## 5. 子作用域析构五阶段(`~Context`)

拆除一个作用域(含每次 stopRecord 的 scope.reset() 与宿主作用域自然析构)严格按以下阶段, 顺序不可变:

| 阶段 | 动作 | 目的 |
| --- | --- | --- |
| 0. 静默 | `m_alive` 令牌复位 | 后续一切观察者回调(dep/service watcher)经 `weak_ptr` 判定失活, 不再进入——先断回调再动状态 |
| 1. 记录 | 逐条**逆序** stop(目标 Disposed); 每条之后若有Owned库立即 `unload()`; 释放其依赖哨兵; 拆除抛异常被捕获并记日志, 级联继续 | 后装载的先拆(逆序≈依赖反序); 单个插件拆除失败不毒化整树 |
| 1b. 退役库 | 全部记录停止后, 清空 `m_retiredLibraries`(真正 dlclose) | 保证此刻树上不再有任何活对象引用退役库代码(§8) |
| 2. 服务哨兵 | 释放本作用域自身的 service watch | — |
| 3. 订阅 | Blob 订阅与事件连接经 RAII 句柄释放 | — |
| 4. 撤服务 | **逆序** revoke 本作用域 provided 的服务 | 撤销触发下游哨兵→下游再 Deferred; 逆序与装载序对称 |

---

## 6. 依赖活性与重启语义

依赖哨兵(`armDependencyWatchers`)对每个 required 服务各注册一个 watch, 回调只做两件事:

- 服务上线(非 null)且本记录 `Deferred` → `ensureStarted`;
- 服务离线(null)且本记录 `Active` → `stopRecord(Deferred)`。

时序示例(`A requires svc.x`, `B provides svc.x`):

```
t0  ctx.plugin("A", meta{required:["svc.x"]}, applyA)
      → log loaded → Deferred(svc.x 缺席, 哨兵在哨)
t1  ctx.plugin("B", …) → B.Active, provide("svc.x")
      → A 的哨兵观测到上线 → ensureStarted(A)
      → A.Active: 子作用域#1 + applyA + log activated
t2  B 被依赖重启/卸载, svc.x 撤销
      → A 的哨兵观测 null → stopRecord(A, Deferred)
      → 子作用域#1 五阶段拆除: 服务/订阅/实例全灭 → A.Deferred
t3  任何插件再次 provide("svc.x")
      → A 重新激活: 子作用域#2 + 全新实例 + 重新 applyA + log activated
```

即 **CORDIS 依赖活性**: 拆除可传递(B 拆 → A 挂起), 重启可再激活(A 拿到全新世界), 激活之间零共享状态、零泄漏。细则(设计 §6):

1. 晚绑定: 装载顺序无关;
2. 依赖面向**服务 id 契约**而非插件名(测试源/设备源可互换);
3. 多依赖全部满足才启动, 任一失效即整体拆回 Deferred;
4. 依赖环: 环上全员常驻 Deferred, `topology()` 可观测(环检测列 M4 未做);
5. 嵌套即子图: 父记录拆除级联其子作用域内全部记录。

---

## 7. 三条终止路径对比

| | 依赖重启 | 管理卸载 `unload(name)` | 宿主作用域析构 `~Context` |
| --- | --- | --- | --- |
| 目标状态 | `Deferred`(可再激活) | `Disposed` 且**记录遗忘**(查名即 Disposed) | `Disposed`, 全树 |
| 实例/服务/订阅 | 全灭 | 全灭 | 全灭 |
| Own 动态库 | **驻留**(仅实例销毁) | 转入退役列表, 驻留 | 先逐记录 stop+unload, 再清退役列表, 最终 dlclose |
| 依赖哨兵 | 保留(等再激活) | 释放 | 释放 |
| 下游影响 | 撤销服务→下游再 Deferred | 同左 | 同左(顺序逆装载序) |
| 可恢复性 | 提供者回归即自动重启 | 需重新 `plugin()`/`loadAppConfig` | — |

---

## 8. 动态插件(.so)时间线与退役库纪律

```
dlopen ──► resolve entry ──► G1 握手 ──► 建档 ──► (Deferred→)Active
                                                        │  每次激活 def->create()
                                       管理卸载/依赖失效 │  每次 stop def->destroy()
                                                        ▼
                              实例死亡, 代码驻留(退役列表 m_retiredLibraries)
                                                        │
                                          ~Context(宿主作用域死亡)
                                                        ▼
                                     全部记录停止后 dlclose —— 唯一卸库时机
```

**为什么实例死了库不能卸**(实测教训, 设计 §7.3): 消费者手里可能仍握有跨 DSO 服务对象的 `shared_ptr`——对象本身已由 `def->destroy()` 正确销毁, 但**只要有任何逃逸对象/静态析构延迟引用库内代码**(vtable、typeinfo、内联体), 提前 `dlclose` 就是把可执行内存抽走。因此:

- `SharedLibrary::unload()` 只在 `~Context` 执行(Phase 1 每记录 + Phase 1b 退役列表);
- 依赖重启只销毁实例, 库驻留——重启风暴成本 = N 次 create/destroy, 0 次 dlopen;
- **禁止任何外部 dlclose**(包括 QLibrary 对象先行析构): 收养形态(`pluginFromDef`)正是为此存在——外部加载器保活库, 内核只管实例。

---

## 9. 销毁纪律与反模式清单

| # | 纪律 | 违反后果 |
| --- | --- | --- |
| 1 | 宿主永不 `delete` 插件实例 | 动态形态必须 `def->destroy()`(配对插件侧 operator new); 对象形态归调用方; 类形态归 `record->owned` |
| 2 | 永不外部 `dlclose`/卸库 | 悬垂 vtable → 拆除期崩溃(实测: 注册表残留 + 分派到未映射内存) |
| 3 | `dispose()` 不得抛异常 | 内核会捕获+日志, 但这掩盖了插件 bug; dispose 应只做幂等收尾 |
| 4 | 不要跨激活期缓存子作用域指针/裸服务引用 | 依赖重启后子作用域与实例全部换代; 正确姿势: `watch()` 订阅重取, 或每次 apply 重建 |
| 5 | Failed 后不要等自动重试 | Failed 是终态(防异常风暴); 恢复=修复后重新 `plugin()` |
| 6 | `apply` 内不要 provide 自己 requires 的服务(自环) | 全员常驻 Deferred, 用 `topology()` 诊断 |
| 7 | 不要在拆除路径(dispose/析构)里再调用宿主 API | 重入由"先置态"守卫, 但行为未定义; 拆除路径只允许收尾自身资源 |
| 8 | 事件 lambda 捕获作用域引用要绑定接收者生命周期 | Qt 宿主: receiver 用 widget(connection 自动断); 纯内核: 作用域订阅 `ctx.on` 自管, 手动 `bus().on` 需自管 RAII 句柄 |
| 9 | 内核头变更(增删成员/内联体)必须重编全部插件 | 陈旧 .so 静默写错成员槽位(实测); G1 握手 + T8/T17 往返用例是护栏 |

---

## 10. 线程模型

生命周期全程**宿主线程**(`Context`/`ServiceRegistry`/`EventBus` 无锁假设)。唯一跨线程入口是 `BlobChannel::post()`(生产者线程投帧, 宿主线程 `pumpAll()` 排水)。因此:

- `plugin()/unload()/pluginState()` 等全部生命周期 API 禁止从非宿主线程调用;
- apply/哨兵回调/dispose 均在宿主线程同步执行, 无并发面;
- Qt 宿主的跨线程数据请在适配层自行 marshal(参考宿主 `adapters/qt.h` 的 PumpTimer 模式)。

---

## 11. 可观测性

**日志契约**(`Log.h`, 宿主线程同步输出):

| 日志 | 时机 | 备注 |
| --- | --- | --- |
| `plugin loaded: 'n' v… (kernel …, abi …)` | 每次 addRecord | 装载事件, 与激活分开记账 |
| `plugin deferred: 'n' …` | 建档后依赖未满足 | Deferred 哨兵开始 |
| `plugin activated: 'n' …` | **每次**激活 | 含依赖重启的再激活(运维需要每个真实生命周期事件) |
| `plugin 'n' threw: …` | apply 异常 | 随后进入 Failed |
| `plugin 'n' dispose threw: …` / `teardown threw` | 收尾异常 | 被内核隔离, 级联继续 |

**内省 API**(全部零异常):

- `pluginState(name)`: 四态; 未知名 = `Disposed`;
- `pluginError(name)`: 最近一次激活异常(F6 诊断), 空串=无记录;
- `pluginVersion(name)` / `pluginRequires(name)`: 版本仅审计展示, 装载决策只认 abiVersion;
- `lastLoadedName()/lastLoadedVersion()`: 最近一次建档(为 loadAppConfig 汇总服务, 勿建逻辑);
- `topology()`: 树形文档 `{name,label,version,state,requires,error,provides,children}`——环/挂起诊断的第一现场。

---

## 12. 宿主集成: 拆除顺序契约

root `Context` 必须在 `main()` 中**后于宿主窗口声明**——C++ 逆序析构 ⇒ root **最先**拆除(宿主 SpectrumExplorerDemo 即此布局: `MainWindow window;` 在前, `ccordis::Context root;` 在后):

```cpp
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    MainWindow window;          // 先声明 → 后析构(部件树完整存活到 root 拆完)
    ccordis::Context root;      // 后声明 → 先析构: 插件实例/服务/订阅此刻清空
    root.plugin<…>(…);          // 装配
    window.setCentralWidget(root.inject<QWidget>("ui.root").get());
    return app.exec();
}   // 析构序: root(内核五阶段拆除) → window(部件树死亡, 无内核可触碰)
```

理由(两个方向都危险, 该布局同时避开):

- **若 root 最后拆**(声明在前): 插件类形态实例在 `owned.clear()` 时析构, 而它们持有的 widget 指针已随窗口悬垂——实例析构触碰部件即 UAF;
- **若部件先于内核登记物死亡**(外部提前析构窗口): 部件析构若触碰内核侧资源(事件发射、blob 投递)同样悬垂。

即: **内核登记物(实例/订阅/服务)先死, 部件树后死**, 双方在对方死亡时都不再互相触碰。

同一条规则的推论——**任何持有 `Context&` 的宿主对象必须先于该 Context 死亡**:

```cpp
ccordis::Context ctx;          // 先声明
ccordis::qt::PumpTimer pump(ctx);   // 后声明 → 先析构: 定时器不会打进死内核
```

`tst_Ccordis` 的 PumpTimer 用法即此序; 反过来(PumpTimer 先声明)则 Qt 定时器可能在 ctx 死亡后触发 `m_ctx.pumpAll()` → UAF。宿主侧装配模式(双轨开关、容器注入)见插件指南 §2/§6。
