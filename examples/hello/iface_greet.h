// tests/iface_greet.h —— 跨DSO服务接口共享头(T17往返测试)
// 宿主(tests)与插件(plugins/hello)必须共享此单一头文件,
// CCORDIS_INTERFACE 锚定唯一 typeinfo, 否则 inject 因严格校验返回 nullptr。
#ifndef IFACE_GREET_H
#define IFACE_GREET_H
#include <ccordis/Export.h>     // 由 INCLUDEPATH 指向 ccordis/ 目录解析
#include <string>
class CCORDIS_INTERFACE IGreet {
public:
    virtual ~IGreet() = default;
    virtual std::string greet() = 0;
};
#endif
