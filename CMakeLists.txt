cmake_minimum_required(VERSION 3.20)

# ── 版本单一事实源: include/ccordis/Export.h ────────────────────────────────
# CCORDIS_VERSION  → 项目版本 (libccordis.so.<VERSION>)
# CCORDIS_ABI_VERSION → SOVERSION/soname (libccordis.so.<ABI>) —— 与内核
# PluginDef::abiVersion 握手契约同源, 升 ABI 必须同步 Export.h。
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/include/ccordis/Export.h" _ccordis_export_h)
string(REGEX MATCH "#define CCORDIS_VERSION \"[0-9]+\\.[0-9]+\\.[0-9]+\"" _ver_line "${_ccordis_export_h}")
string(REGEX REPLACE "#define CCORDIS_VERSION \"([0-9]+\\.[0-9]+\\.[0-9]+)\"" "\\1" CCORDIS_PROJECT_VERSION "${_ver_line}")
string(REGEX MATCH "#define CCORDIS_ABI_VERSION [0-9]+" _abi_line "${_ccordis_export_h}")
string(REGEX REPLACE "#define CCORDIS_ABI_VERSION ([0-9]+)" "\\1" CCORDIS_PROJECT_ABI "${_abi_line}")

project(ccordis
    VERSION "${CCORDIS_PROJECT_VERSION}"
    DESCRIPTION "Minimal C++17 plugin kernel (context / event-bus / service-registry / blob-channel)"
    LANGUAGES CXX)

# 宿主 SpectrumExplorerDemo 的 Qt 测试(tst_PluginRegistry T8/T17)按构建树
# 路径装载 examples/hello/libccordis_hello.so, 故默认参与构建。
option(CCORDIS_BUILD_TESTS    "Build ccordis standalone smoke tests" ON)
option(CCORDIS_BUILD_EXAMPLES "Build bundled example plugins (hello)" ON)
option(CCORDIS_BUILD_SHARED_LIBS "Build libccordis as a shared library" ON)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ── 内核共享库 ────────────────────────────────────────────────────────────────
if(CCORDIS_BUILD_SHARED_LIBS)
    add_library(ccordis SHARED)
else()
    add_library(ccordis STATIC)
endif()
add_library(ccordis::ccordis ALIAS ccordis)

target_sources(ccordis PRIVATE
    src/Blob.cpp
    src/BlobChannel.cpp
    src/BlobFrame.cpp
    src/Context.cpp
    src/EventBus.cpp
    src/Json.cpp
    src/Log.cpp
    src/PluginManifest.cpp
    src/PluginRegistry.cpp
    src/RequestTracker.cpp
    src/ServiceRegistry.cpp
    src/SharedLibrary.cpp)

set_target_properties(ccordis PROPERTIES
    VERSION   "${PROJECT_VERSION}"
    SOVERSION "${CCORDIS_PROJECT_ABI}")

target_compile_features(ccordis PUBLIC cxx_std_17)
set_target_properties(ccordis PROPERTIES CXX_EXTENSIONS OFF)

target_include_directories(ccordis
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

# 注意: 内核公共类未逐类标注导出宏(Export.h 仅约定 CCORDIS_EXPORT 插件入口
# 与 CCORDIS_INTERFACE 接口锚点), 因此内核 SO 必须保持默认可见性, 否则宿主
# 链接不到任何符号(qmake 时代宿主内嵌源码, 该问题从未暴露)。ABI 边界由
# "soname 跟随 CCORDIS_ABI_VERSION + 头文件即契约" 约束, 见 Export.h。
target_compile_definitions(ccordis PRIVATE CCORDIS_BUILD)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(ccordis PRIVATE -Wall -Wextra)
endif()

find_package(Threads REQUIRED)
target_link_libraries(ccordis
    PUBLIC  Threads::Threads
    PRIVATE ${CMAKE_DL_LIBS})

# ── 安装与导出 ────────────────────────────────────────────────────────────────
install(TARGETS ccordis
    EXPORT ccordisTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(DIRECTORY include/ccordis/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/ccordis
    FILES_MATCHING PATTERN "*.h")

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/ccordis-config-version.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

configure_package_config_file(
    cmake/ccordis-config.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/ccordis-config.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ccordis)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/ccordis-config.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/ccordis-config-version.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ccordis)

install(EXPORT ccordisTargets
    NAMESPACE ccordis::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ccordis)

# 导出构建树供 add_subdirectory()/FetchContent 消费
export(EXPORT ccordisTargets
    NAMESPACE ccordis::
    FILE "${CMAKE_CURRENT_BINARY_DIR}/ccordisTargets.cmake")

# ── 子目录 ────────────────────────────────────────────────────────────────────
if(CCORDIS_BUILD_EXAMPLES)
    add_subdirectory(examples/hello)
endif()
if(CCORDIS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
