# ccordis/ccordis_lib.pro
#
# ccordis 内核共享库 —— 产品态链接目标(Gap G8)。
#
#   cd ccordis && qmake ccordis_lib.pro && make -j
#   产物: ccordis/libccordis.so
#
# 用途:
#   - 宿主 exe 链接 -lccordis 后, 动态插件(.so)无需再内嵌内核源,
#     运行时符号由共享库统一提供(替代开发态 -rdynamic 方案);
#   - 内核 ABI 版本(CCORDIS_ABI_VERSION)即该 SO 的 SONAME 契约。
#
# 依赖: 纯 C++17 标准库 + dlopen/pthread(系统库), 零 Qt 依赖。

TEMPLATE = lib
TARGET   = ccordis
CONFIG  += c++17 shared plugin
QT       =                                  # 零 Qt 依赖
QMAKE_CXXFLAGS += -fvisibility=hidden       # 仅导出 C 接口/显式标记的符号
# soname 跟随 ABI: abi 2 (PluginDef::version 追加) → libccordis.so.2
QMAKE_LFLAGS   += -Wl,-soname,libccordis.so.2

# 导出宏: 构建 SO 时符号导出, 使用时导入
DEFINES += CCORDIS_BUILD

INCLUDEPATH += $$PWD

HEADERS += \
    Export.h \
    Value.h \
    Blob.h \
    BlobFrame.h \
    BlobChannel.h \
    EventBus.h \
    ServiceRegistry.h \
    Plugin.h \
    RequestTracker.h \
    SharedLibrary.h \
    Log.h \
    Context.h

SOURCES += \
    Blob.cpp \
    BlobFrame.cpp \
    BlobChannel.cpp \
    EventBus.cpp \
    ServiceRegistry.cpp \
    SharedLibrary.cpp \
    Log.cpp \
    Context.cpp \
    RequestTracker.cpp

target.path = $$OUT_PWD
INSTALLS   += target
