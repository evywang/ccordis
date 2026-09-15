// tests/test_kernel.cpp —— ccordis 独立冒烟测试(零 Qt)。
//
// 只验证"作为独立共享库可以被外部项目链接并完成最小闭环";
// 完整行为矩阵在宿主 SpectrumExplorerDemo 的 Qt 测试套件里。
#include <ccordis/Context.h>
#include <ccordis/EventBus.h>
#include <ccordis/Export.h>
#include <ccordis/Json.h>
#include <ccordis/Plugin.h>
#include <ccordis/ServiceRegistry.h>
#include <ccordis/Value.h>

#include <cstdio>
#include <string>

static int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,     \
                         #cond);                                             \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

using namespace ccordis;

namespace {

struct DemoPlugin {
    DemoPlugin(Context &ctx, const Value &cfg)
    {
        const std::string who = cfg.at("who").toString("kernel");
        ctx.provide<std::string>("svc.who", std::make_shared<std::string>(who));
    }
};

} // namespace

int main()
{
    // 版本/ABI 契约: 宿主与插件据此互信(G1)
    CHECK(std::string(version()) == std::string(CCORDIS_VERSION));
    CHECK(CCORDIS_ABI_VERSION == 2);

    // Context: 装配 + 服务 + 事件
    {
        Context ctx;
        int hits = 0;
        ctx.plugin("demo.main", PluginMeta{"demo.main", "", {}, {"svc.who"}},
                   [&](Context &c, const Value &cfg) {
                       c.plugin<DemoPlugin>("demo.inner", cfg, {});
                       c.on("app.ready", [&](const Value &) { ++hits; });   // 作用域订阅
                   });
        auto who = ctx.inject<std::string>("svc.who");
        CHECK(who && *who == "kernel");
        CHECK(ctx.inject<int>("svc.who") == nullptr);   // 错型→null (T2)

        // 配置驱动覆盖默认值
        Value cfg = Value::object({{"who", "smoke"}});
        CHECK(cfg.at("who").toString("x") == "smoke");

        ctx.emitEvent("app.ready");
        ctx.emitEvent("app.ready");
        CHECK(hits == 2);

        // 卸载 → 服务撤销 + 作用域订阅自动退订
        ctx.unload("demo.main");
        CHECK(ctx.inject<std::string>("svc.who") == nullptr);
        ctx.emitEvent("app.ready");
        CHECK(hits == 2);   // 不再增长
    }

    // EventBus: FIFO 顺序承诺(F9)—— on() 返回 RAII 句柄, 语句结束即退订, 需保活
    {
        EventBus bus;
        std::string order;
        auto c1 = bus.on("fifo", [&](const Value &) { order += "1"; });
        auto c2 = bus.on("fifo", [&](const Value &) { order += "2"; });
        bus.emitEvent("fifo", Value());
        CHECK(order == "12");
        c1.release();
        c2.release();
    }

    // Json → Value
    {
        Value v;
        std::string err;
        CHECK(parseJson(R"({"name":"ccordis","abi":2})", v, &err));
        CHECK(v.at("name").toString("") == "ccordis");
        CHECK(!parseJson("{oops", v));
    }

    if (g_failures == 0) {
        std::printf("test_kernel: all checks passed (kernel %s, abi %d)\n",
                    version(), CCORDIS_ABI_VERSION);
        return 0;
    }
    std::printf("test_kernel: %d check(s) FAILED\n", g_failures);
    return 1;
}
