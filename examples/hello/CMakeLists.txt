# examples/hello —— 最小动态插件示例 (libccordis_hello.so)
#
# 发布三铁律(插件指南 §5.3):
#   1. 与宿主同一 ccordis 头文件版本(ABI 握手 abiVersion 兜底拒载, G1);
#   2. 产品态链接 -lccordis(G8 libccordis.so), 运行期与宿主共享同一内核实例;
#   3. 卸载只经 ctx.unload()(退役库纪律 §7.3)——禁止外部 dlclose。
#
# iface_greet.h 为宿主/插件跨 DSO 共享接口头, 宿主侧测试按本目录相对路径引用。
add_library(ccordis_hello MODULE hello_plugin.cpp)

target_compile_features(ccordis_hello PRIVATE cxx_std_17)
set_target_properties(ccordis_hello PROPERTIES CXX_EXTENSIONS OFF)

# 只导出 ccordis_plugin_entry
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(ccordis_hello PRIVATE -fvisibility=hidden -Wall -Wextra)
endif()

target_include_directories(ccordis_hello PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ccordis_hello PRIVATE ccordis::ccordis)
