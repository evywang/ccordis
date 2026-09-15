# ccordis — minimal C++17 plugin kernel

纯 C++17 插件内核(零 Qt / 零第三方依赖):`Context` 装配 + `EventBus` 事件总线 +
`ServiceRegistry` 服务注册 + `BlobChannel` 大数据帧通道 + 动态插件装载
(`PluginRegistry` / manifest / `SharedLibrary`)。

## 目录结构

```
ccordis/
├── CMakeLists.txt            根构建(版本/SOVERSION 从 Export.h 解析)
├── cmake/                    find_package 包配置模板
├── include/ccordis/          公共头(全部 15 个, 安装为 <ccordis/*.h>)
├── src/                      内核实现
├── examples/hello/           最小动态插件示例 libccordis_hello.so
│   └── iface_greet.h         宿主/插件跨 DSO 共享接口头
├── tests/                    独立冒烟测试(零 Qt)
└── doc/                      设计/评审/指南全套文档
    ├── plugin-lifecycle.md   插件生命周期详解(状态机/转移/拆除五阶段/退役库纪律)
    ├── ccordis-plugin-system-design.md   完整设计方案 v2.1
    ├── ccordis-plugin-guide.md           插件作者实操手册
    ├── ccordis-selection-report.md       技术选型调研
    ├── ccordis-design-review.md          设计评审报告
    └── ccordis-gap-analysis*.md          缺口审查(两轮)
```

## 构建

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
ctest --test-dir build            # 冒烟测试
```

产物(构建树):

- `build/libccordis.so.2.2.0`(+ `.so.2` soname 链接 + `.so` 开发链接)
- `build/examples/hello/libccordis_hello.so`(示例插件, 宿主 Qt 测试 T8/T17 按此路径装载)

ABI 即 soname:`CCORDIS_ABI_VERSION`(`include/ccordis/Export.h`)升版时,
SOVERSION 随之升版, 旧插件由内核 abiVersion 握手兜底拒载(G1)。

## 安装与 find_package

```bash
cmake --install build --prefix /usr/local   # 或任意前缀
```

消费方:

```cmake
find_package(ccordis 2.2 REQUIRED)
target_link_libraries(myapp PRIVATE ccordis::ccordis)
```

或源内集成:`add_subdirectory(ccordis)` / FetchContent, 目标名同为 `ccordis::ccordis`。

## 宿主 SpectrumExplorerDemo(qmake)消费方式

demo 不再内嵌内核源码, 链接本项目的共享库(产品态, 原 Gap G8 方案):

```bash
# 1) 先构建本库
cmake -S code/ccordis -B code/ccordis/build && cmake --build code/ccordis/build -j
# 2) 再构建 demo / tests(qmake 会检查 build/libccordis.so 存在并 -lccordis 链接)
cd code/SpectrumExplorerDemo && qmake && make -j$(nproc)
```

- 头文件路径:`INCLUDEPATH += ../ccordis/include`,`#include <ccordis/Context.h>`
- 链接:`-L../ccordis/build -lccordis`,RPATH 已指向构建树,无需 LD_LIBRARY_PATH

## 构建选项

| 选项 | 默认 | 说明 |
|------|------|------|
| `CCORDIS_BUILD_TESTS` | ON | 独立冒烟测试 |
| `CCORDIS_BUILD_EXAMPLES` | ON | hello 示例插件(宿主 Qt 测试依赖其构建树路径) |
| `CCORDIS_BUILD_SHARED_LIBS` | ON | OFF 时构建静态库 |
