// plugins/hello/hello_plugin.cpp
#include <string>
#include "Plugin.h"
#include "Context.h"
#include "Log.h"
#include "iface_greet.h"   // 跨DSO共享接口(宿主与插件同头, CCORDIS_INTERFACE锚定)
namespace {
class HelloGreet : public IGreet {
public:
    std::string greet() override { return "hello-from-dynamic-plugin"; }
};
class HelloPlugin : public ccordis::IPlugin {
    void apply(ccordis::Context &ctx, const ccordis::Value &config) override {
        const std::string who = config.at("who").toString("dynamic-plugin");
        ctx.provide<IGreet>("hello.greeting", std::make_shared<HelloGreet>());
        ctx.on("app.ready", [who](const ccordis::Value &) { ccordis::log("hello from %s", who.c_str()); });
    }
};
}
CCORDIS_PLUGIN_DEF_V(HelloPlugin, "demo.hello", "1.0.0")
