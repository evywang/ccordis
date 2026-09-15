# ccordis 插件系统补充项审查（第二轮，架构师视角）

> 迁移说明 (2026-09-15): 本文档随 ccordis 独立化从 `SpectrumExplorerDemo/doc/` 迁入本项目 `doc/`。文中旧路径对照: 内核代码 `ccordis/` → `include/ccordis/`(公共头) + `src/`(实现); `plugins/hello` → `examples/hello`; 生命周期细节见 `plugin-lifecycle.md`。

> 审查人角色: 系统高级架构师
> 审查基线: 提交 46f914d —— 含内核 v2.1、ccordis 更名、GUI 容器协议、帧头重设计（BLOB + 类型扩展区）、G1/G2/G4 交付
> 审查性质: **演进后二次扫描**。首轮审查（`ccordis-gap-analysis.md`，P0 四项）中 G1/G2/G4 已落地；本轮立足现状识别残余缺口，按"阻断集成 / 生产加固 / 规模演进"三档重排优先级。
> 结论先行: **内核核心已闭合、工程护栏基本就位；当前最大风险是"文档详备而代码链路未贯通"——帧头有精密定义却无一行解析代码、容器协议有完整契约却无宿主桥、主程序从未接入。建议下一阶段以"贯通"为主线：帧头解析器 + adapters/qt 桥 + M1 宿主接线，把纸面协议变成可跑链路。**

---

## 1. 本轮已关闭项（首轮 P0，不再重复建设）

| 项 | 首轮 | 本轮 |
| --- | --- | --- |
| G1 ABI 版本握手 | 高 | ✓ 已交付（Export.h 宏 + PluginDef.abiVersion + 加载比对拒载，含垃圾值实证） |
| G4 事件风暴防护 | 高 | ✓ 已交付（重入上限 16 + 日志丢弃） |
| G2 回归入库 | 高 | ✓ 已交付（tst_Ccordis 12 用例接 tests.pro + plugins/hello + iface_greet.h，42 用例全绿 + ASAN 干净） |
| 内核更名 cordis→ccordis | — | ✓ 已交付 |

---

## 2. 新增缺口（本轮工作引入）

### 新-A 帧头定义精密却无解析代码
- **位置**: 设计 §9.3 帧头（32B，含 type/subType/ext 类型相关解释表）
- **现状**: 表定义了六种数据类型的 ext 区语义，但**仓库内无任何代码**读取/填充/校验该头。生产者写 Blob 时靠手工 memcpy，消费者解析靠 reinterpret_cast 硬编码偏移——出错成本极高。
- **风险**: 帧头是数据面事实标准，首行生产代码写错一个偏移即全链路静默错乱；且 type=0x04 IQ 的 ext 布局与 0x01 频谱不同，手工解析极易遗漏分支。
- **建议**: 落地 `BlobFrameHeader`  POD 结构（32B，`#pragma pack(1)`）+ `serialize(host, out[32]) / deserialize(in[32])` 两个纯函数（字节序显式转大端）+ `validMagic()` 校验。归入 ccordis 内核（纯 C++17，无 OS 依赖，守 §0 红线）。工作量 ~0.5 人日。

### 新-B 容器协议有契约无宿主桥
- **位置**: 指南 §2.1 IUiContainer 契约 + 装配协议
- **现状**: 协议经 offscreen 冒烟验证（/tmp/uismoke），但 `IUiContainer` 头文件、`SplitContainer` 示例、`RootFacade` 均未入库；且 offscreen 冒烟本身也未进仓库（G2 未覆盖 GUI 用例）。
- **风险**: M2 宿主接入时每个开发者各自重写一遍容器桥，易偏离已验证的构造顺序规则①（先挂父容器再 provide）。
- **建议**: （a）`adapters/qt/layout/` 落一个可复用的 `SplitContainer` + `TabContainer` + `DockContainer` 参考实现（头文件即可，不必进内核）；（b）offscreen 容器冒烟并入 tst_Ccordis 或独立 `tst_CcordisLayout`。工作量 ~1.5 人日。

### 新-C 扩展区类型表与 Value 命令封套无 schema 校验约定
- **位置**: §9.3 命令封套 `{"type","SET_BAND","id","params",...}`
- **现状**: 文档给出示例，但未定义"合法命令 schema"——必填字段、值域、未知名是否拒收。
- **风险**: 协议插件各自解释命令封套，同一命令名在不同插件里字段名微有差异（`min`/`lo`、`max`/`hi`）即集成失败。
- **建议**: 在指南或独立头中给出"命令封套 schema 约定"表（每条命令的必填/选填/类型/值域），作为协议插件开发的 input contract，由 `DeviceCommandService` 实现类在入口处校验。工作量 ~0.3 人日（文档）。

---

## 3. 残余缺口（首轮 P1/P2，重排优先级）

### 第一档——阻断真实集成（M1/M2 前置）

**G3b 主程序从未接入（最大风险）**
- 首轮列为"当前最大风险"，至今**未动**——SpectrumExplorerDemo 的 main.cpp / MainWindow 仍以旧方式运行，ccordis 内核零覆盖于真实应用。
- 纸面验证再充分 ≠ 真实链路成立：内存布局、对象寿命、Qt 事件循环交互、信号线程 hop，只有真跑才暴露。
- **建议**: M1 立刻执行，最小化切入——main() 建 root Context + 注册 model.core + 一个视图插件先跑通，再铺开。哪怕只接 1% 代码，风险折旧速率即逆转。

**G3a adapters/qt 桥缺失**
- QString↔string / QJson↔Value / QTimer-pump 集成 / QueuedConnection 回程——指南里全是应然，无一存在。M2 每一步重复造轮且易错。
- **建议**: 首轮计划不变，M2 前交付。

**G8 libccordis.so 共享库形态**
- 当前动态插件靠 `-rdynamic` 开发态方案，产品态需内核编为共享库供宿主与插件共链。
- **建议**: qmake 增 `ccordis_lib.pro`（TEMPLATE=lib），与 `hello.pro` 共测。工作量 ~0.5 人日。

### 第二档——生产加固（M2/M3）

**G5 装载期安全钩子**
- 首轮 F7 记录在案。设备产品离线交付，`setLoadValidator(fn)` 留宿主挂哈希白名单。
- **建议**: 内核留回调接口（~5 行），策略在宿主。

**G6 可观测聚合**
- 通道/超时各有 stats 但无统一出口；插件失败仅 topology 可查（lastError），无事件广播。
- **建议**: `Context::stats()` 聚合 + `plugin.failed` 事件（载荷含 plugin 名 + lastError）。UI 可据此弹诊断。

**G7 配置热重载**
- 设备调机改参数不改代码是刚需。`reload(name, newCfg)` = unload + 按新配置重载（保留记录位置与依赖边）。

**G9 GUI 线程亲和守护（debug）**
- debug 构建下记录根 Context 线程 ID，关键 API（provide/inject/on/publish）入口比对。把 §12 军规变成可执行检查。

### 第三档——规模演进（按需，<20 插件标注"轻量即可"）

- **G10 慢消费者模式**: 邮箱仅丢旧，补 per-subscriber 回调耗时统计（识别谁拖 pump）。
- **G11 基准套件**: 微基准进 CI 带 regression 阈值。
- **G12 插件包格式**: 目录约定 + 版本字段即可，**勿造包管理器**。
- **G14 崩溃隔离**: 仅第三方闭源插件需求出现时立项（同仓同编无此问题）。
- **G15 doxygen / CMake**: 生态配套，远期。

---

## 4. 一张图：从"纸面"到"跑通"的贯通路线

```
当前状态                              目标
─────────                            ────
帧头定义(文档§9.3)  ──→ 新-A ──→  BlobFrameHeader POD + 序列化/反序列化 ──→ 数据面协议跑通
容器协议(指南§2.1)  ──→ 新-B ──→  adapters/qt/layout 参考实现 + 冒烟入库  ──→ GUI 装配跑通
内核(已验证)        ──→  ──────→   ┐
命令封套(示例)      ──→ 新-C ──→  schema 校验约定表                    ──→ 协议插件可互换
                    ──→ G3b ──→  M1 主程序接入（哪怕 1%）              ──→ ★ 真实链路成立 ★
                                   G3a ──→  adapters/qt 桥
                                   G8  ──→  libccordis.so
```

> ★ 为全阶段风险解除的标志性节点。

---

## 5. 行动建议（按依赖排序）

| 序号 | 行动 | 对应 | 工作量 | 理由 |
| --- | --- | --- | --- | --- |
| A1 | 帧头 POD + 序列化/反序列化函数 | 新-A | 0.5d | 数据面首行代码即复用、消除手工偏移硬编码 |
| A2 | 命令封套 schema 约定表 | 新-C | 0.3d | 协议插件开发的 input contract |
| A3 | M1 主程序最小接入（root Context + 1 视图） | G3b | 1.5d | **全阶段最大风险解除** |
| A4 | adapters/qt 桥 + 容器参考实现 + 冒烟入库 | G3a + 新-B | 2d | M2 前置，消除重复造轮 |
| A5 | libccordis.so + hello 共测 | G8 | 0.5d | 产品态链接形态 |
| B1 | 装载期校验回调 | G5 | 0.2d | 设备产品安全入口 |
| B2 | stats 聚合 + plugin.failed 事件 | G6 | 0.5d | 生产可观测 |
| B3 | 配置热重载 reload(name, cfg) | G7 | 0.5d | 设备调机刚需 |
| B4 | debug 线程亲和断言 | G9 | 0.2d | 军规可执行化 |

---

*本轮审查基于 46f914d；新-A/B/C 为帧头重设计与容器协议引入的首轮未覆盖缺口。"贯通"是本轮关键词——把已验证的纸面协议变成可跑代码链路。*
