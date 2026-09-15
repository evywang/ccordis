#ifndef CCORDIS_PLUGIN_H
#define CCORDIS_PLUGIN_H

#include <ccordis/Export.h>
#include <ccordis/Value.h>

#include <cstdint>
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
    std::string version = "0.0.0";      // semantic version (audit/display only)
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
    std::uint32_t abiVersion;         // must equal CCORDIS_ABI_VERSION (G1)
    IPlugin *(*create)();             // factory
    void (*destroy)(IPlugin *);       // matching deleter (never null)
    const char *version = nullptr;    // "major.minor.patch"; nullptr = "0.0.0".
                                      // Trailing (abi 2): audit/display only —
                                      // load decisions stay with abiVersion.
};

/** Kernel release string (ccordis::version()). */
inline const char *version() { return CCORDIS_VERSION; }

/** Decode a PluginDef's static char* metadata into PluginMeta. */
inline PluginMeta metaFromDef(const PluginDef *def)
{
    PluginMeta meta;
    if (!def)
        return meta;
    meta.name = def->name ? def->name : "";
    meta.label = def->label ? def->label : "";
    meta.version = def->version ? def->version : "0.0.0";
    if (def->reqs) {
        for (const char *const *r = def->reqs; *r; ++r)
            meta.required.emplace_back(*r);
    }
    return meta;
}

} // namespace ccordis

/** Shared expansion core; do not use directly. VER is the semantic version
 *  string embedded in the descriptor (nullptr-able via "0.0.0" default). */
#define CCORDIS_PLUGIN_DEF_IMPL(CLASS, NAME, VER, REQ_ARRAY)                       \
    static ccordis::IPlugin *ccordis_create_impl() { return new (CLASS)(); }       \
    static void ccordis_destroy_impl(ccordis::IPlugin *p) { delete p; }            \
    extern "C" CCORDIS_EXPORT const ccordis::PluginDef *ccordis_plugin_entry() {    \
        static const ccordis::PluginDef ccordis_def = {                            \
            NAME, nullptr, REQ_ARRAY, CCORDIS_ABI_VERSION,                         \
            &ccordis_create_impl, &ccordis_destroy_impl, VER                       \
        };                                                                       \
        return &ccordis_def;                                                      \
    }

/** Declare a dynamic plugin with no service requirements (version "0.0.0"). */
#define CCORDIS_PLUGIN_DEF(CLASS, NAME)                                            \
    static const char *ccordis_req_##CLASS[] = { nullptr };                        \
    CCORDIS_PLUGIN_DEF_IMPL(CLASS, NAME, "0.0.0", ccordis_req_##CLASS)

/**
 * Declare a dynamic plugin requiring the listed service ids (version "0.0.0").
 * Usage: CCORDIS_PLUGIN_DEF_REQ(MyPlugin, "demo.x", "demo.dep1", "demo.dep2")
 */
#define CCORDIS_PLUGIN_DEF_REQ(CLASS, NAME, ...)                                   \
    static const char *ccordis_req_##CLASS[] = { __VA_ARGS__, nullptr };           \
    CCORDIS_PLUGIN_DEF_IMPL(CLASS, NAME, "0.0.0", ccordis_req_##CLASS)

/** Versioned variant: CCORDIS_PLUGIN_DEF_V(MyPlugin, "demo.x", "1.4.2") */
#define CCORDIS_PLUGIN_DEF_V(CLASS, NAME, VER)                                     \
    static const char *ccordis_req_##CLASS[] = { nullptr };                        \
    CCORDIS_PLUGIN_DEF_IMPL(CLASS, NAME, VER, ccordis_req_##CLASS)

/** Versioned + requires:
 *  CCORDIS_PLUGIN_DEF_REQ_V(MyPlugin, "demo.x", "1.4.2", "demo.dep1") */
#define CCORDIS_PLUGIN_DEF_REQ_V(CLASS, NAME, VER, ...)                            \
    static const char *ccordis_req_##CLASS[] = { __VA_ARGS__, nullptr };           \
    CCORDIS_PLUGIN_DEF_IMPL(CLASS, NAME, VER, ccordis_req_##CLASS)

#endif // CCORDIS_PLUGIN_H
