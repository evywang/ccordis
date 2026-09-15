# CORDIS 理念插件系统技术选型调研报告

> 迁移说明 (2026-09-15): 本文档随 ccordis 独立化从 `SpectrumExplorerDemo/doc/` 迁入本项目 `doc/`。文中旧路径对照: 内核代码 `ccordis/` → `include/ccordis/`(公共头) + `src/`(实现); `plugins/hello` → `examples/hello`; 生命周期细节见 `plugin-lifecycle.md`。

> 状态: 调研定稿 (2026-09-14)
> 范围: 为 `SpectrumExplorerDemo`（及后续无线电侦察设备软件族）选定一个**可商用**的 C++ 插件化基础库，并以 CORDIS 插件理论为蓝本构建完整插件系统
> 理论来源: [cordis.moe — 插件的基本形式](https://cordis.moe/zh-CN/guide/essential/index.html)
> 关联文档: `ccordis-plugin-system-design.md`（完整设计方案，本报告的工程落地）

---

## 1. 背景与目标

### 1.1 需求来源

CORDIS（cordisjs，MIT 许可证）是 Koishi 生态提炼出的 TS 插件框架，其核心理论可归纳为五条：

1. **插件是基本构造单元**：功能以插件形式解耦、分发（函数 / 类 / 带 `apply` 的对象三种形态，签名统一为 `(ctx, options)`）；
2. **Context 即服务容器**：插件拿到的是一个树形作用域上下文，从其中注入服务、注册服务；
3. **依赖声明式管理**：插件声明 `requires`，依赖不满足时挂起，满足后自动启动；依赖消失时自动销毁并重新挂起（重启语义）；
4. **嵌套组合**：插件可以加载子插件，形成树；销毁作用域即级联卸载子树；
5. **配置驱动 + 具名可视化**：插件可由配置文件装载，插件关系图可导出可视化。

本仓库需要把这一理论移植到 **C++ / Qt 5.15** 工具链上，支撑：

- 频谱多视图演示程序（专利 `多视图频谱显示方法及系统` 的参考实现）的后端可替换（模拟源 ↔ 真实设备源）；
- 已启动的内部插件化演进（P1 频谱导航条四平面分离 `nav/`、风格预设 `theme/`）向**真正的插件系统**收敛；
- 后续设备软件族的模块分发（插件独立编译、按配置装载）。

### 1.2 调研目标

回答三个问题：

- **Q1**: C++ 生态中有没有"基于该插件理论、可商用"的现成开源库？
- **Q2**: 它与本项目工程约束（qmake / C++11 应用层 / Qt 5.15 / 离线构建）的匹配度如何？
- **Q3**: 最终采用什么方案落地，风险与升级路径是什么？

### 1.3 工程约束（硬性）

| 约束 | 现状 | 来源 |
| --- | --- | --- |
| 构建系统 | qmake（非 CMake） | `SpectrumExplorerDemo.pro` |
| 语言标准 | 应用层 C++11，测试层 C++17 | `.pro` / `tests/tests.pro` |
| Qt | 5.15.13（已链接 core/gui/widgets/network） | `qmake --version` |
| 离线性 | 不依赖包管理器在线装库（Boost 无开发头文件） | 本机核查：仅 `libboost-*-1.83.0` 运行库，无 `/usr/include/boost` |
| 专利载荷 | 调度时序常数/公式是权利要求，改动需对齐 `PATENT_APPLICATION_CN_v10.md` 并更新 `PATENT_MAPPING.md` | 仓规 |
| 商用许可 | 所有引入组件必须允许闭源商用分发 | 需求方要求 |

---

## 2. 调研方法

五个评估维度，权重按本项目实情排序：

| 维度 | 权重 | 说明 |
| --- | --- | --- |
| D1 许可证商用性 | ★★★★★ | 是否 MIT / Apache-2.0 / Boost-1.0 / LGPL(动态链接)级别 |
| D2 理论契合度 | ★★★★★ | 是否覆盖 CORDIS 五要素：三形态、服务容器、依赖重启、嵌套作用域、配置装载 |
| D3 工程契合度 | ★★★★ | qmake/C++11/Qt5.15/离线约束下的引入成本 |
| D4 成熟度 | ★★★ | 社区规模、维护活跃度、实战案例 |
| D5 侵入性 | ★★★ | 是否要求改写现有架构、是否劫持主循环 |

数据来源：各项目 README/LICENSE 原文（GitHub raw / API 实测抓取，2026-09-14）、CORDIS 官方文档、本机工具链核查。

---

## 3. 候选库逐一调研

### 3.1 CppMicroServices —— 理论上的最佳匹配

**基本信息**（GitHub API 实测，2026-09-14）：

| 项 | 值 |
| --- | --- |
| 仓库 | https://github.com/CppMicroServices/CppMicroServices |
| 定位 | "An OSGi-like C++ dynamic module system and service registry" |
| 许可证 | **Apache License 2.0**（闭源商用友好，含专利授权条款） |
| 活跃度 | 879 stars / 291 forks；2012-02 创建；最近推送 2026-09-02（**仍在活跃维护**） |
| 语言标准 | C++17（GCC≥7.5 / Clang≥9 / MSVC≥2017） |
| 构建系统 | CMake ≥ 3.17，自带 `usResourceCompiler` 资源编译器生成 Bundle 清单 |
| 出身 | 德国癌症研究中心（DKFZ）孵化，医学影像生态（MITK）在用 |

**理论与 CORDIS 的同源性**：CppMicroServices 是 **OSGi 规范的 C++ 实现**，而 CORDIS 的"Context 服务容器 + 插件生命周期 + 依赖注入"正是 OSGi 模型在 JS 生态的再实现。概念对照：

| CORDIS | CppMicroServices / OSGi |
| --- | --- |
| `Context` | `BundleContext` |
| `ctx.provide/inject` | `RegisterService` / `GetServiceReference` |
| 依赖 `requires` + 晚绑定 | `ServiceTracker`（服务跟踪器） |
| 插件 `apply(ctx, options)` | `BundleActivator::Load(BundleContext)` |
| 插件销毁 | `BundleActivator::Unload` |
| `ctx.plugin()` 嵌套 | Bundle 依赖树 + `Framework` 事件 |
| 具名插件可视化 | Bundle/Service 状态查询 API |

**优点**：理论覆盖度最全（D2 满分）；Apache-2.0；十四年维护史；动态 Bundle（.so）热装热卸、服务注册表、服务跟踪器都是生产级实现。

**风险与成本（针对本工程）**：

- **R1 构建体系冲突**：Bundle 的清单/资源需 `usResourceCompiler` 在 CMake 宏里编译，qmake 工程需手工模拟该步骤，构建链脆弱（D3 低）。
- **R2 语言标准**：要求全量 C++17，应用层现为 C++11（可升，但波及整个应用 TU 的编译行为）。
- **R3 体积与离线**：完整引入需 vendor 整个框架源码并自建（本机可离线 git clone，但 CI/其他开发机重复成本高）。
- **R4 语义过重**：OSGi 的 Bundle 版本协商、资源系统、框架事件对本 demo 是过度能力。

### 3.2 Boost.DLL —— 正确的底层原语，缺上层理论

| 项 | 值 |
| --- | --- |
| 仓库 | https://github.com/boostorg/dll |
| 许可证 | Boost Software License 1.0（商用最宽松级别） |
| 定位 | "comfortable work with DLL and DSO" —— 动态库加载与符号导入（`import<T>()`、`shared_library`） |

**评估**：它解决的是"从 .so 导入工厂函数"这一层，正是 CORDIS 插件系统需要的加载原语；但**不提供** Context、服务注册表、依赖重启、作用域树——理论五要素只覆盖"配置装载"的底层一半。且**本机无 Boost 开发头文件**（§1.3），引入需先装 `libboost-dev`，违反离线约束（D3 差）。

### 3.3 Qt 自带插件机制（QPluginLoader / QLibrary）

| 项 | 值 |
| --- | --- |
| 许可证 | LGPLv3（**动态链接方式下闭源商用可行**，本项目已整体基于该模式使用 Qt） |
| 定位 | `QPluginLoader`：带元数据（Q_PLUGIN_METADATA）的 Qt 插件装载；`QLibrary`：裸 `dlopen/LoadLibrary` 封装 |

**评估**：零新增依赖（D3 满分）。`QPluginLoader` 的 IID/元数据约定偏 Qt 生态；`QLibrary` 则是干净的 C 符号解析层，与 Boost.DLL 的 `shared_library` 同构。与 CppMicroServices 一样，**不含**服务容器/依赖重启等理论层。

### 3.4 Poco（SharedLibrary + ClassLoader）

Boost-1.0 许可证，`Poco::ClassLoader<T>` 提供"清单式插件类工厂"（比 Boost.DLL 略高一层）。代价是引入整个 Poco 基础库（网络/日志/线程全家桶），对本 demo 严重过度（D5 差），排除。

### 3.5 排除项说明

- **QtCharts 类比**：与本选题无关（绘图库非插件框架）；
- **dyno / C++ 反射类库**：解决运行时多态，非插件分发；
- **直接移植 CORDIS（TS）**：语言运行时模型不同（GC/模块热替换），无法字面移植，仅取理论。

---

## 4. 决策矩阵

| 候选 | D1 许可证 | D2 理论契合 | D3 工程契合 | D4 成熟度 | D5 侵入性 | 结论 |
| --- | --- | --- | --- | --- | --- | --- |
| CppMicroServices | ●●●●● Apache-2.0 | ●●●●● OSGi 全要素 | ●● CMake+C++17 | ●●●●● 14 年 | ●● 框架接管度高 | **语义参考实现（选型锚点）** |
| Boost.DLL | ●●●●● Boost-1.0 | ●● 仅加载层 | ●● 本机无头文件 | ●●●● Boost 官方 | ●●●●● | 备选（若已有 Boost 则优先） |
| Qt QLibrary | ●●●● LGPL 动态链接 | ●● 仅加载层 | ●●●●● 已在栈内 | ●●●●● Qt 官方 | ●●●●● | 宿主适配层可用；**内核因去 Qt 红线排除** |
| 自研 SharedLibrary | ●●●●● 仓内原创 | ●● 仅加载层 | ●●●●● ~百行，零依赖 | — | ●●●●● | **加载层落地选择（v2）** |
| Poco | ●●●●● Boost-1.0 | ●●● 清单式工厂 | ●● 整库引入 | ●●●● | ●● | 排除 |

---

## 5. 选型结论（分层采用）

**结论：以 CppMicroServices 为语义参考实现（理论锚点），在仓内实现 CORDIS 语义内核 `ccordis/`，动态加载层采用 Qt `QLibrary`。**

### 5.1 论证

1. **"基于该理论的可商用库"存在且唯一成熟**：CppMicroServices（Apache-2.0）。CORDIS 与 OSGi 同源，它是 C++ 生态里对该理论最完整、最长寿的生产级实现——选型在**理论层面**锚定于此。
2. **工程层面不可整体引入**（§3.1 R1–R4）：qmake 离线工程直接嵌入其 CMake Bundle 工具链，构建脆弱度与收益不成比。
3. **理论内核本身很薄**：CORDIS 内核（Context/服务容器/依赖重启/作用域树）在 C++ 中约千行量级，且 C++ 的 RAII 恰好把 CORDIS 需要显式 `dispose()` 的资源回收变成语言机制——自研内核不是重复造轮子，而是理论的**语言级再表达**。
4. **加载层自研 `SharedLibrary`（dlopen/LoadLibrary 封装，约百行）**：v2 设计确立"内核零框架依赖"红线（只依赖 C++17 标准库，见设计方案 §0）——Qt QLibrary 因会把 Qt 带进内核接口而被排除；Boost.DLL 与之同构但本机无开发头文件。自研封装与两者 API 同构，宿主若已有任一依赖可一行替换。
5. **内核语言标准定为 C++17**（`std::variant`/`std::filesystem` 地基）：各平台支持矩阵已核实（GCC≥9 / Clang≥7 / VS2019 16.x / Xcode≥11 全量可用），本机 g++ 13.3 默认即 gnu++17，实测零障碍。

### 5.2 落地形态

```
┌────────────────────────────────────────────────────────┐
│  宿主应用（Qt / 纯C++ / 服务进程…）+ 业务插件             │
├────────────────────────────────────────────────────────┤
│  adapters/（宿主桥，如 Qt: QString/QJson ↔ 内核类型）    │
├────────────────────────────────────────────────────────┤
│  ccordis/ 内核（纯 C++17，零框架依赖）                     │  ← 设计见 ccordis-plugin-system-design.md (v2)
│  Context · ServiceRegistry · EventBus · Plugin · Value  │
├────────────────────────────────────────────────────────┤
│  OS 触点: SharedLibrary（自研 dlopen/LoadLibrary 封装）  │
├────────────────────────────────────────────────────────┤
│  语义参考: CppMicroServices（Apache-2.0，不链接）        │  ← API 语义对照见设计方案附录 A
└────────────────────────────────────────────────────────┘
```

### 5.3 许可证合规

- **不链接、不复制 CppMicroServices 代码**：仅参照其公开文档的 API 语义（Apache-2.0 对文档参照无约束；实现为独立表达），无 NOTICE/传染义务；
- **QLibrary**：Qt 以动态链接使用，沿用项目既有 LGPL 合规方式，不新增义务；
- **CORDIS 理论**：MIT，文档引用注明出处即可；
- 仓内 `ccordis/` 内核随仓库现有版权策略发布。

### 5.4 B 方案（直接采用 CppMicroServices）升级路径

当以下**任一**条件成立时，重估直接嵌入：

- 目标产品需要**跨进程/多框架实例**或 Bundle 版本协商；
- 工程整体迁移到 CMake + C++17；
- 需要 C++ 生态现成的 Bundle 工具链与资源系统。

**PoC 验证计划**（触发时执行，预计 2 人日）：

| # | 任务 | 退出标准 |
| --- | --- | --- |
| P1 | clone + CMake 构建 CppMicroServices 静态库 | 本机 g++13 一次通过 |
| P2 | 手工用 qmake 编译一个最小 Bundle（`US_EXPORT_BUNDLE_ACTIVATOR` + 手动 `usResourceCompiler`） | qmake 链路可复现 |
| P3 | 将 `SignalSimulator` 后端改装为该 Bundle 并热装卸 | 装载→出数据→卸载无残留 |
| P4 | ABI 压测：同版本工具链编译宿主/插件，往返装/卸 1000 次 | 无泄漏（ASAN 干净） |

---

## 6. 遗留风险登记

| 风险 | 等级 | 对策 |
| --- | --- | --- |
| 动态插件 ABI 依赖同工具链（编译器/Qt/STL 一致） | 中 | 文档强制约束 + CI 加 ABI 冒烟（设计方案 §13） |
| 自研内核的边角语义（依赖环、重入销毁）未被覆盖 | 中 | `tst_Ccordis` 专项用例（设计方案 §13） |
| 专利载荷被插件化误伤（调度时序常数） | 高 | **核心调度不插件化**，仅为服务；改动走 `PATENT_MAPPING.md` 对齐流程（设计方案 §11.4） |
| SharedLibrary 卸载时机不当导致悬垂 vtable | 中 | 仅宿主作用域死亡时卸载（设计方案 §7.3） |
| 内核去 Qt 红线被后续提交破坏 | 高 | CI 纯净性用例：无 Qt include 路径编译内核（设计方案 §13 T0） |
| app 升 C++17 的存量破坏（register/throw() 等移除项） | 中 | 升级前全量编译回归；vpxproto 头已实测可过 |

---

*附：本报告数据抓取脚本与原始 JSON 存档于调研过程（2026-09-14，GitHub API / raw.githubusercontent.com 实测）。*
