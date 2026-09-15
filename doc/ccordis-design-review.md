# ccordis 插件系统设计方案 评审报告

> 迁移说明 (2026-09-15): 本文档随 ccordis 独立化从 `SpectrumExplorerDemo/doc/` 迁入本项目 `doc/`。文中旧路径对照: 内核代码 `ccordis/` → `include/ccordis/`(公共头) + `src/`(实现); `plugins/hello` → `examples/hello`; 生命周期细节见 `plugin-lifecycle.md`。

> 评审人角色: 系统高级架构师
> 评审对象: `ccordis-plugin-system-design.md`(v2) + 已落地内核代码（`ccordis/`）
> 评审基准: 现代 C++ 设计理念（C++ Core Guidelines、RAII/所有权、类型安全、并发契约）与经典/现代设计模式
> 日期: 2026-09-14
> 结论先行: **方向正确、结构成熟，予以通过；存在 1 项高优先级修正（F1 并发契约）与 1 项经实证降级的可移植性防御条款（F2），建议 M0 收尾前处置。**

---

## 1. 评审范围与方法

- 文档: v2 设计（含 §0 依赖红线、§4 模块、§7 ABI、§9 数据传输、§9.2 超时分层）；
- 代码: 纯 C++17 已实现件（Blob/BlobPool、BlobChannel、RequestTracker，冒烟全绿）与 v1 Qt 地基件（EventBus/ServiceRegistry/Plugin/Context，待 v2 平移）；
- 方法: 逐节契约审查 + 代码交叉验证（引用行号）+ 模式适配性评估。

## 2. 总体评价

| 维度 | 评价 |
| --- | --- |
| 架构分层 | **优**。控制面/数据面分离、内核/宿主桥分离、OS 触点单点化（SharedLibrary.cpp）是教科书式分层 |
| 现代 C++ 实践 | **良**。RAII 全面替代显式 dispose、move-only 句柄、variant 类型擦除、PIMPL 化池状态、可注入时钟 |
| 并发契约 | **中**。大方向正确（双跳纪律、快照分发），但有一处**线程契约与实现矛盾**（F1）与内存序未成文（F4） |
| ABI/互操作 | **良**。C-ABI 描述符、单 owner 卸载、收养 API；缺 RTTI 跨 DSO 说明（F2） |
| 可测试性 | **优**。注入时钟、mock 服务互换、T0 纯净性护栏、16 项用例矩阵 |
| 演进风险 | **中**。服务无版本协商（有意取舍）、动态插件供应链安全未提（F7） |

发现计: **高 1 / 中 6 / 低 6**（F2 经实证由高降为中）。

## 3. 优点确认（值得保持的决策）

1. **§0 依赖红线 + T0 护栏**——"约束靠 CI 不靠自觉"，多数项目做不到；
2. **RAII 作用域树**——CORDIS 的显式 dispose 在 C++ 里被析构顺序四阶段（令牌→记录→watch→连接→revoke）替代，且用存活令牌解决拆除风暴重入，这是本设计最有价值的一笔；
3. **依赖即服务 id 契约**——面向契约编程使 mock/真实后端互换（M2 验收依赖此）；
4. **控制/数据面分离 + 有界邮箱丢最旧**——对活流（频谱帧）的背压选择正确，seq 跳变测丢帧是廉价可观测性；
5. **BlobPool 共享 State 模式**——块删除器捕获 `shared_ptr<State>`，池亡后释放安全，PIMPL+生命周期解耦的标准解法；
6. **超时三层分离**——跟踪器纯数据结构（tick 驱动、可注入时钟）、策略在 facade、体验在视图；`forget()` 使统计反映结果而非尝试次数，细节见功力；
7. **§11.4 专利红线**——把领域约束（权利要求载荷）写进架构文档的变更门槛。

## 4. 发现与建议

### F1【高】`setPumpHook` 线程契约与实现矛盾
- **位置**: `BlobChannel.cpp:64-65`、`BlobChannel.h:131`、设计 §4.7 表
- **事实**: `post()` 在**生产线程**内联调用 `m_pumpHook()`；而头文件注释写 "host-thread callback"、设计表把该行线程列为"宿主线程"——契约与实现相反。
- **影响**: 按现文档，Qt 宿主会在 hook 里 `QTimer::start()`——定时器亲和 GUI 线程，从设备线程启动属未定义行为；这是会把偶发崩溃埋进现场的高危陷阱。
- **建议**: ①把契约改为"**在 post 调用线程执行，只许做无锁唤醒调度**"，Qt 侧标准姿势是 `QMetaObject::invokeMethod(guiObj, pump, Qt::QueuedConnection)` 或 eventfd/pipe 唤醒；②修正头注释与 §4.7 表；③T14 增补"hook 在生产线程被调"的断言用例。

### F2【中·已实证降级】跨 DSO 的 `typeid` 可见性——本工具链安全，可移植性防御仍需条款
- **位置**: `Context.h:139,146`、设计 §7.2
- **实证（2026-09-14，本机 g++ 13.3/libstdc++）**: hidden 插件与宿主各持一份 `type_info`（地址不同、name 串不同址），`operator==` **仍判定 EQUAL**——libstdc++ 内联实现带 `strcmp(name)` 回退，`std::type_index` 与 `std::hash` 均基于 name 串。故本平台跨 DSO 注入**不会**静默失配。
- **残余风险**: libc++（macOS/Xcode 同源）等实现以指针比较为主、回退受限，hidden 可见性下存在失配报告（本机无 libc++ 开发环境，未能实测）。
- **建议（防御性条款，保留）**: §7.2 仍强制"跨 DSO 服务接口类型 default visibility（或 key function 锚定）"；T17 双 .so 注入往返用例照立——它同时是 ABI 冒烟。原始"高"级判定因实证降为"中"。

### F3【中】异常传播策略未定义（EventBus / dispose / teardown）
- **位置**: `EventBus.cpp`（emit 无 catch）、`Context.cpp:322-325`（仅 apply 有 catch）
- **事实**: `apply` 抛异常被捕获（好）；但 **EventBus 处理器异常**会沿 `emitEvent` 穿透到调用方——若发生在拆除级联（revoke→watcher→stopRecord）中途，将跳过后续拆除阶段造成状态不一致；`dispose()` 抛异常同理会跳过 `def->destroy()` 与库卸载。
- **建议**: ①内核级策略成文：*处理器/dispose 约定不抛；内核在分发与拆除边界 catch-all + logger 记录*；②`~Context` 各阶段以 noexcept 包装兜底；③T1/T6 增补"emit 中处理器抛异常不中断其余订阅者"用例。

### F4【中】Blob 不可变契约与内存序（happens-before）未成文
- **位置**: 设计 §4.7、`Blob.h`
- **事实**: 跨线程路径（设备线程填充→`post`→宿主 `pump`→N 消费者读）目前正确**仅因为**邮箱互斥提供了 release/acquire；这一正确性依赖是隐式的。`wrap()` 外部内存的别名风险（写者在 post 后继续写）无任何防护或文档。
- **建议**: §4.7 增"内存模型"小节：①明确"单写者在 **post/publish 之前**完成全部写入；post 的互斥释放与 pump 的获取构成 happens-before"；②`wrap` 文档强调"交出后不得再写"；③可选强化：debug 构造下给池块加 canary/generation 校验。

### F5【中】`provide<QWidget>` 的所有权模型与 Qt 父子所有权冲突
- **位置**: 设计 §4.3、§11.1
- **事实**: `shared_ptr<QWidget>` 进注册表后，Qt 的 parent 所有权与 shared_ptr 删除器形成双 owner；且视图插件化后若依赖 F2 条款未落实，跨 DSO 注入 QWidget 服务会踩 F2。
- **建议**: 适配层规范定稿为"**注册非拥有视图**"：`shared_ptr<QWidget>(w, null_deleter)`，Qt 父子所有权唯一负责销毁；shared_ptr 仅作保活句柄；该规则写入 §10（宿主适配层）而非内核。

### F6【中】Failed 终态无诊断载荷
- **位置**: 设计 §5.1、`topology()`
- **事实**: 插件启动失败仅状态位，现场排障拿不到异常消息。
- **建议**: `LoadRecord` 增 `lastError`（string），`topology()` 节点带 `error` 字段；配合既定 logger 注入点。

### F7【中】动态插件供应链安全（设备产品语境）
- **位置**: 设计 §7、§8（`lib:` 路径来自配置文件）
- **事实**: 无线电侦察设备是实体交付产品；`loadConfig` 从文件取 `.so` 路径直接装载等于任意代码执行入口。
- **建议**: M4 前至少文档声明"插件目录属受控资产"；正式产品加装载前哈希白名单/签名校验（内核留校验回调钩子即可，策略在宿主）。

### 低优先级（记录在案）

| # | 发现 | 建议 |
| --- | --- | --- |
| F8 | Service Locator 与构造期 DI 的可测试性取舍未说明 | 测试规范: Context 作 fixture 注入 fake；可加 `injectOr<T>(id, fallback)` |
| F9 | 同事件多处理器分发顺序未承诺 | 明确承诺"订阅序 FIFO"（现实现即是）或声明 unspecified |
| F10 | `ccordis/` 内 v1(Qt) 与 v2(纯 C++17) 并存漂移 | 按既定 todo 尽快平移重写，v1 文件删除而非并存 |
| F11 | EventBus 与 BlobChannel 订阅机制重复（token/快照两套） | 抽公共模板基类或接受小规模重复（当前规模可接受） |
| F12 | publish 每次快照拷贝 std::function | SBO 下通常无堆分配；仅记录，暂不优化 |
| F13 | Value::Object 用 std::map 丢失插入序；键序不稳定影响 diff/序列化 | 如需稳定序改 vector<pair>；配置场景影响小 |

## 5. 设计模式映射（评估适配性）

| 模式 | 落点 | 评价 |
| --- | --- | --- |
| Service Locator | Context+ServiceRegistry | 有意选择（CORDIS 语义），测试性代价已被 F8 建议覆盖 |
| Observer / Pub-Sub | EventBus；Registry watchers；BlobChannel | 三观察面职责需一句决策规则：**事件=已发生的事实；watcher=能力的出现/消失**（建议写入 §4.2） |
| State | 插件四态状态机 | 转移表完备，先置状态再行动的重入防护正确 |
| Factory + Factory Registry | registerFactory + PluginDef::create | 双工厂（进程内/跨 ABI）统一到 ApplyFn，收敛得好 |
| Adapter | adapters/qt | 依赖方向正确（宿主依赖内核） |
| Facade | DeviceCommandService | UI 与网络的隔离点，超时/重试边界清晰 |
| Bridge/PIMPL | BlobPool::State | 并发+生命周期双赢 |
| RAII Handle / Scope Guard | ScopedConnection/ScopedSubscription/Context 析构 | 全篇灵魂，正确 |
| Object Pool | BlobPool（幂次桶+上界） | 稳态零分配已实测 |
| Strategy | 超时/重试策略表 | 层位正确（facade 而非内核） |

## 6. 现代 C++ 实践核对（Core Guidelines 摘项）

| 项 | 状态 |
| --- | --- |
| R.1 资源管理即 RAII | ✅ 全篇；无裸 new/delete 跨界（PluginDef 配对纪律） |
| Rule of Five（move-only 句柄） | ✅；小瑕疵: EventBus::ScopedConnection 移动未标 noexcept（顺手修） |
| CP.20/CP.31 并发契约成文 | ⚠️ F1/F4 补齐后达标 |
| T.40 算法分配意识 | ✅ 快照/池化取舍均有理由 |
| ES 系（无悬垂视图） | ✅ Blob keep-alive 模式；seen 指针类用法在测试中受控 |
| API 设计（explicit、deleted copy、[[nodiscard]]） | 基本 ✅；建议 `inject/publish` 系列补 [[nodiscard]] |
| I.11 编译期防误用 | ✅ 错型 inject 返回 nullptr（T2 护住）；F2 修后跨 DSO 同理 |

## 7. 行动项（按优先级）

| # | 行动 | 对应 | 时机 |
| --- | --- | --- | --- |
| A1 | 修正 pumpHook 契约（注释+文档+T14 断言），Qt 桥用 QueuedInvoke 唤醒 | F1 | **立即（M0）** |
| A2 | §7.2 增 RTTI 可见性防御条款（已实证本平台安全）+ T17 跨 DSO 往返用例 | F2 | M0 |
| A3 | 异常策略成文 + 分发/拆除边界 catch-all | F3 | M0 |
| A4 | §4.7 内存模型小节（happens-before） | F4 | M0 |
| A5 | 适配层"非拥有视图"规范 | F5 | M2 前 |
| A6 | topology 带 lastError + logger 落地 | F6 | M2 |
| A7 | 供应链校验钩子（策略留宿主） | F7 | M4 评审 |
| A8 | ASAN/TSAN 进 CI（数据面+通道压测） | — | M1 |
| A9 | v1 内核平移重写（既定 todo） | F10 | M0 |

---

*本报告基于 2026-09-14 文档与代码状态；F1/F3/F4 的代码引用已逐行核实。*
