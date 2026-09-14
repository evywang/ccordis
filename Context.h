#ifndef CCORDIS_CONTEXT_H
#define CCORDIS_CONTEXT_H

#include "Blob.h"
#include "BlobChannel.h"
#include "EventBus.h"
#include "Plugin.h"
#include "ServiceRegistry.h"
#include "Value.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ccordis {

class SharedLibrary;

/**
 * @brief Hierarchical service context — the C++ counterpart of the cordis
 *        `Context` (design §4.6/§4.8). A Context is simultaneously:
 *
 *   - a service scope: services provided through it are revoked when it
 *     dies; subscriptions/watchers unsubscribe automatically (RAII);
 *   - a plugin host: plugin() loads into a child scope; destroying the
 *     parent tears down the whole subtree;
 *   - a dependency node: a plugin whose required services are missing stays
 *     Deferred and starts when they appear; when a requirement disappears
 *     the plugin is torn down and re-deferred (cordis restart semantics).
 *
 * Threading: host thread only (§12). The single cross-thread exception in
 * the whole kernel is BlobChannel::post() from producer threads.
 */
class Context
{
public:
    enum class State { Deferred, Active, Failed, Disposed };

    using ApplyFn = std::function<void(Context &, const Value &)>;
    using Middleware = std::function<void(EventBus::Payload &,
                                          std::function<void()>)>;

    /** Create the application root context (owns registry/bus/pool). */
    Context();
    ~Context();

    Context(const Context &) = delete;
    Context &operator=(const Context &) = delete;

    // ── plugin loading (three cordis forms + dynamic) ──────────────────────

    /** Function form: apply(ctx, config) is the plugin body. */
    void plugin(const std::string &name, const PluginMeta &meta,
                const ApplyFn &apply, const Value &config = Value());

    /**
     * Class form — cordis `class { constructor(ctx, options) }`:
     * T(Context&, const Value&) IS the plugin body; the instance is bound
     * to the load record and destroyed at (every) teardown.
     */
    template <typename T>
    void plugin(const std::string &name, const Value &config = Value(),
                const std::vector<std::string> &required = {})
    {
        registerFactory(name, PluginMeta{name, name, required, {}},
                        [this](Context &scope, const Value &cfg) {
                            pushClassOwned<T>(currentRecord(), scope, cfg);
                        });
        loadRegistered(name, config);
    }

    /** Object form: pre-built IPlugin; ownership stays with the caller. */
    void plugin(const std::string &name, IPlugin *instance,
                const Value &config = Value(),
                const std::vector<std::string> &required = {});

    /**
     * @brief Load a plugin from a shared library exporting
     *        ccordis_plugin_entry() (§7.1). The library is owned by this
     *        scope and unloaded only at its teardown (§7.3).
     * @return true when resolved and registered (may still be Deferred).
     */
    bool pluginFromLibrary(const std::string &path,
                           const Value &config = Value());

    /**
     * @brief Adopt a plugin whose library was loaded by a FOREIGN loader
     *        (QLibrary/Boost.DLL/manual dlopen — §7.6 interop): the host
     *        resolved ccordis_plugin_entry() itself and hands over the def.
     *        The kernel never unloads an adopted library.
     */
    bool pluginFromDef(const PluginDef *def, const Value &config = Value());

    /**
     * @brief Admin unload: stop a plugin (teardown cascade) and forget its
     *        record. Revoking its services re-defers downstream consumers.
     */
    bool unload(const std::string &name);

    State pluginState(const std::string &name) const;

    /** Last activation error (review F6); empty when none recorded. */
    std::string pluginError(const std::string &name) const;

    // ── config-driven bootstrap ────────────────────────────────────────────

    /** Register an in-process factory addressable by name. */
    void registerFactory(const std::string &name, const PluginMeta &meta,
                         const ApplyFn &apply);

    /** Load a plugin by factory name (config-driven path). */
    bool loadRegistered(const std::string &name, const Value &config = Value());

    /**
     * @brief cordis-style config document:
     *   { "plugins": [ {"plugin": "demo.x", "options": {...}},
     *                  {"plugin": "lib:plugins/libcordis_hello.so"} ] }
     * Unknown names are logged and skipped, never fatal.
     */
    void loadConfig(const Value &doc);

    // ── services (cordis provide/inject) ───────────────────────────────────

    template <typename T>
    void provide(const std::string &name, const std::shared_ptr<T> &impl)
    {
        m_providedNames.push_back(name);   // revoke list for this scope
        registry().provide(name, typeid(T), impl, this);
    }

    /** Inject a service; nullptr when absent or type-mismatched. */
    template <typename T>
    std::shared_ptr<T> inject(const std::string &name) const
    {
        return ServiceCast<T>::from(registry().inject(name, typeid(T)));
    }

    /** Watch a service for this scope (reactive inject, §4.3). */
    template <typename T>
    void watch(const std::string &name,
               const std::function<void(const std::shared_ptr<T> &)> &cb)
    {
        std::weak_ptr<bool> alive = m_alive;
        m_watchTokens.push_back(registry().watch(
            name, [alive, cb](const std::shared_ptr<void> &erased) {
                if (alive.lock())
                    cb(ServiceCast<T>::from(erased));
            }));
    }

    /** Non-template watch for type-erased consumers. */
    void watchAny(const std::string &name, const ServiceRegistry::Watcher &cb);

    // ── events (cordis on/emit) ────────────────────────────────────────────

    /** Scope-bound subscription — auto-released when this scope dies. */
    void on(const std::string &event, const EventBus::Handler &handler);
    void emitEvent(const std::string &event,
                   const EventBus::Payload &payload = EventBus::Payload());

    // ── middleware (cordis ctx.middleware) ─────────────────────────────────

    /** Append (default) or prepend to this scope's chain. */
    void middleware(const Middleware &mw, bool prepend = false);

    /** Run a payload through the chain; not calling next() short-circuits. */
    void run(EventBus::Payload &payload);

    // ── data plane (§4.7/§9) ───────────────────────────────────────────────

    /** Lazily create/lookup a named Blob channel (shared across the tree). */
    std::shared_ptr<BlobChannel> channel(const std::string &name);

    /** Scope-bound Blob subscription (auto-detached on teardown). */
    void onBlob(const std::string &name, const BlobChannel::Handler &h);

    /** Drain every channel's mailbox on the host thread (host loop tick). */
    void pumpAll();

    /** Shared blob pool (root-owned); producers allocate frames from it. */
    BlobPool &blobPool();

    // ── introspection ──────────────────────────────────────────────────────

    /** Plugin tree as a Value document (visualization data source, §4.6). */
    Value topology() const;

    ServiceRegistry &registry() { return *m_registry; }
    const ServiceRegistry &registry() const { return *m_registry; }
    EventBus &bus() { return *m_bus; }

    bool isRoot() const { return m_parent == nullptr; }
    Context *parent() { return m_parent; }

private:
    struct LoadRecord {
        PluginMeta meta;
        ApplyFn apply;
        Value config;
        State state = State::Deferred;
        std::shared_ptr<Context> scope;          // child scope while started
        std::vector<ServiceRegistry::WatchToken> depWatchTokens;
        std::string lastError;                   // activation diagnostics
        // activation-owned, reset on every stop:
        std::vector<std::shared_ptr<void>> owned; // class-form instances
        IPlugin *objectInstance = nullptr;        // object/dynamic form
        bool callDispose = false;
        // hosting-owned, released when the host scope dies:
        PluginDef *def = nullptr;                 // dynamic form
        std::unique_ptr<SharedLibrary> library;   // owned; null when adopted
    };

    Context(Context *parent);                    // child scope ctor

    LoadRecord *currentRecord();
    LoadRecord *findRecord(const std::string &name);

    template <typename T>
    void pushClassOwned(LoadRecord *record, Context &scope, const Value &cfg)
    {
        record->owned.push_back(std::shared_ptr<void>(
            new T(scope, cfg), [](void *p) { delete static_cast<T *>(p); }));
    }

    void ensureStarted(LoadRecord &record);
    void stopRecord(LoadRecord &record, State target);
    bool requiresMet(const std::vector<std::string> &required) const;
    void armDependencyWatchers(LoadRecord &record);
    void addRecord(LoadRecord *rec);

    Context *m_parent;
    std::shared_ptr<ServiceRegistry> m_registry; // shared across the tree
    std::shared_ptr<EventBus> m_bus;             // shared across the tree
    std::shared_ptr<bool> m_alive;               // liveness token (watchers)

    std::vector<std::unique_ptr<LoadRecord>> m_records; // stable addresses
    // Libraries of admin-unloaded plugins: their services/instances die at
    // unload(), but the mapped CODE must stay resident until host-scope
    // teardown (§7.3) — outstanding service shared_ptrs may still hold
    // objects whose vtables live in these libraries.
    std::vector<std::unique_ptr<SharedLibrary>> m_retiredLibraries;
    std::vector<EventBus::ScopedConnection> m_connections;
    std::vector<BlobChannel::ScopedSubscription> m_blobSubs;
    std::vector<ServiceRegistry::WatchToken> m_watchTokens;
    std::vector<std::string> m_providedNames;    // services to revoke on death
    std::vector<Middleware> m_middleware;
    std::map<std::string, std::pair<PluginMeta, ApplyFn>> m_factories;

    std::shared_ptr<BlobPool> m_blobPool;                  // root-owned
    std::shared_ptr<std::map<std::string, std::shared_ptr<BlobChannel>>>
        m_channels;                                        // root-owned

    LoadRecord *m_startingRecord = nullptr;      // during ensureStarted only
};

} // namespace ccordis

#endif // CCORDIS_CONTEXT_H
