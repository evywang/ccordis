#ifndef CCORDIS_PLUGIN_H
#define CCORDIS_PLUGIN_H

#include "Export.h"
#include "Value.h"

#include <string>
#include <vector>

namespace ccordis {

class Context;

/**
 * @brief Plugin contract — C++ translation of cordis' three plugin forms
 *        (design §4.4, https://cordis.moe 插件的基本形式).
 *
 *  cordis form         | C++ equivalent (Context::plugin overloads)
 * -------------------- + -------------------------------------------------
 *  function(ctx, cfg)  | ApplyFn = std::function<void(Context&, const Value&)>
 *  class { ctor }      | class form: plugin<T>(name, config, required);
 *                      |   T(Context&, const Value&) IS the plugin body
 *  object { apply }    | pre-built IPlugin instance (caller keeps ownership)
 *
 * All forms share one lifecycle: apply runs inside a fresh child scope when
 * (and each time) the declared requirements are met; destroying that scope
 * un-registers everything the plugin provided (RAII, design §4.8).
 *
 * NOTE on naming: fields/params are spelled `required`/`reqs`, never
 * `requires` — that is a C++20 keyword and these headers must stay
 * forward-compatible.
 */
class IPlugin
{
public:
    virtual ~IPlugin() {}

    /** Activate inside this plugin's own scope. Re-called after a
     *  dependency restart; register services/events/children via ctx. */
    virtual void apply(Context &ctx, const Value &config) = 0;

    /** Optional teardown hook; must not throw (kernel catches + logs). */
    virtual void dispose() {}
};

/** Static metadata every plugin carries (cordis: named plugins + requires). */
struct PluginMeta
{
    std::string name;                   // unique id, e.g. "source.simulator"
    std::string label;                  // human-readable (visualization)
    std::vector<std::string> required;  // service ids that must be available
    std::vector<std::string> provides;  // intended services (documentation)
};

/**
 * @brief C-ABI descriptor exported by dynamic plugin libraries (§7.1).
 *
 * Only plain pointers cross the library boundary at load time; the IPlugin
 * vtable crosses solely between same-toolchain host/plugin builds (§7.2).
 * The host never delete-expresses the instance — it always calls destroy().
 */
struct PluginDef
{
    const char *name;                 // PluginMeta::name (UTF-8, static)
    const char *label;                // PluginMeta::label, may be nullptr
    const char *const *reqs;          // null-terminated array, may be nullptr
    IPlugin *(*create)();             // factory
    void (*destroy)(IPlugin *);       // matching deleter (never null)
};

/** Decode a PluginDef's static char* metadata into PluginMeta. */
inline PluginMeta metaFromDef(const PluginDef *def)
{
    PluginMeta meta;
    if (!def)
        return meta;
    meta.name = def->name ? def->name : "";
    meta.label = def->label ? def->label : "";
    if (def->reqs) {
        for (const char *const *r = def->reqs; *r; ++r)
            meta.required.emplace_back(*r);
    }
    return meta;
}

} // namespace ccordis

/** Shared expansion core; do not use directly. */
#define CCORDIS_PLUGIN_DEF_IMPL(CLASS, NAME, REQ_EXPR)                           \
    static ccordis::IPlugin *ccordis_create_impl() { return new (CLASS)(); }       \
    static void ccordis_destroy_impl(ccordis::IPlugin *p) { delete p; }            \
    extern "C" CCORDIS_EXPORT const ccordis::PluginDef *ccordis_plugin_entry() {    \
        static const char *ccordis_req[] REQ_EXPR; /* {"a","b"} or {nullptr} */   \
        static const ccordis::PluginDef ccordis_def = {                            \
            NAME, nullptr, ccordis_req, &ccordis_create_impl, &ccordis_destroy_impl \
        };                                                                       \
        return &ccordis_def;                                                      \
    }

/** Declare a dynamic plugin with no service requirements. */
#define CCORDIS_PLUGIN_DEF(CLASS, NAME) \
    CCORDIS_PLUGIN_DEF_IMPL(CLASS, NAME, = { nullptr })

/** Declare a dynamic plugin requiring the listed service ids. */
#define CCORDIS_PLUGIN_DEF_REQ(CLASS, NAME, ...) \
    CCORDIS_PLUGIN_DEF_IMPL(CLASS, NAME, = { __VA_ARGS__, nullptr })

#endif // CCORDIS_PLUGIN_H
