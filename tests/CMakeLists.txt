# tests/ —— 独立冒烟测试(零 Qt、零第三方框架)。
#
# 覆盖内核最小闭环: Context 装配/生命周期、EventBus 作用域订阅、
# ServiceRegistry 错型拒注、Json→Value、版本/ABI 契约。
# 深度行为测试仍由宿主 SpectrumExplorerDemo 的 Qt 测试套件承载
# (tests/tst_Ccordis.cpp 等, 见该仓库 README)。
add_executable(test_kernel test_kernel.cpp)

target_compile_features(test_kernel PRIVATE cxx_std_17)
set_target_properties(test_kernel PROPERTIES CXX_EXTENSIONS OFF)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_kernel PRIVATE -Wall -Wextra)
endif()

target_link_libraries(test_kernel PRIVATE ccordis::ccordis)

add_test(NAME kernel-smoke COMMAND test_kernel)
