# plugins/hello/hello.pro
#
# 动态插件示例 libccordis_hello.so —— 独立构建（插件指南 §5.2 模板）。
#
#   cd plugins/hello && qmake hello.pro && make -j
#   产物: hello/libccordis_hello.so  (tests 的 T8/T17 往返用例加载它)
#
# 发布三铁律（插件指南 §5.3）:
#   1. 与宿主同一 ccordis 头文件版本（ABI 握手 abiVersion 兜底拒载, G1）;
#   2. 插件需解析内核符号——开发态直接链内核源(与宿主 exe 内嵌内核同源,
#      运行期宿主 -rdynamic 导出); 产品态: LIBS += -lccordis (G8 libccordis.so);
#   3. 卸载只经 ctx.unload()（退役库纪律 §7.3）——禁止外部 dlclose。

TEMPLATE = lib
TARGET   = ccordis_hello
CONFIG  += c++17
QT       =                                  # 纯 C++17 插件: 零 Qt 依赖
QMAKE_CXXFLAGS += -fvisibility=hidden       # 只导出 ccordis_plugin_entry
INCLUDEPATH    += ../../ccordis ../../tests

SOURCES += \
    hello_plugin.cpp \
    ../../ccordis/Context.cpp \
    ../../ccordis/EventBus.cpp \
    ../../ccordis/ServiceRegistry.cpp \
    ../../ccordis/SharedLibrary.cpp \
    ../../ccordis/Log.cpp \
    ../../ccordis/Blob.cpp \
    ../../ccordis/BlobChannel.cpp

target.path = $$OUT_PWD
INSTALLS   += target
