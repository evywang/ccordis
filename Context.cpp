#include "Context.h"
#include "Log.h"
#include "SharedLibrary.h"

#include <algorithm>

namespace ccordis {

namespace {
std::string stateToString(Context::State s)
{
    switch (s) {
    case Context::State::Deferred:  return "deferred";
    case Context::State::Active:    return "active";
    case Context::State::Failed:    return "failed";
    case Context::State::Disposed:  return "disposed";
    }
    return "unknown";
}
} // namespace

Context::Context()
    : m_parent(nullptr),
      m_registry(new ServiceRegistry),
      m_bus(new EventBus),
      m_alive(new bool(true)),
      m_channels(new std::map<std::string, std::shared_ptr<BlobChannel>>())
{
}

Context::Context(Context *parent)
    : m_parent(parent),
      m_registry(parent->m_registry),
      m_bus(parent->m_bus),
      m_alive(new bool(true)),
      m_channels(parent->m_channels)
{
}

Context::~Context()
{
    // Phase 0: silence outgoing callbacks (dep watchers, service watchers)
    // before touching any state they might observe.
    m_alive.reset();

    // Phase 1: stop plugin records newest-first; children's destructors
    // recurse through the same phases, tearing down the subtree.
    for (std::size_t i = m_records.size(); i-- > 0; ) {
        LoadRecord &rec = *m_records[i];
        try {
            if (rec.state == State::Active || rec.state == State::Failed)
                stopRecord(rec, State::Disposed);
        } catch (...) {
            log("plugin '%s' teardown threw; continuing cascade",
                rec.meta.name.c_str());
        }
        if (rec.library) {              // hosting-owned; adopted libs: null
            rec.library->unload();      // §7.3: host-scope death only
            rec.library.reset();
        }
        registry().unwatchAll(rec.depWatchTokens);
        rec.depWatchTokens.clear();
    }
    m_records.clear();
    // Retired (admin-unloaded) libraries unload now — after every record
    // stopped, so no live object can reference their code anymore. Objects
    // escaping past this point are the documented "失维护" boundary (§4.3).
    m_retiredLibraries.clear();

    // Phase 2: this scope's own service watchers.
    registry().unwatchAll(m_watchTokens);
    m_watchTokens.clear();

    // Phase 3: event + blob subscriptions release via RAII handles.
    m_blobSubs.clear();
    m_connections.clear();

    // Phase 4: revoke services provided by this scope, newest-first.
    for (std::size_t i = m_providedNames.size(); i-- > 0; )
        registry().revoke(m_providedNames[i], this);
    m_providedNames.clear();
}

// ── plugin loading ─────────────────────────────────────────────────────────

void Context::addRecord(LoadRecord *rec)
{
    m_records.push_back(std::unique_ptr<LoadRecord>(rec)); // stable address
    armDependencyWatchers(*rec);
    // Load-time lifecycle log: registered into this scope. Activation (which
    // may happen now or much later via the Deferred machinery) logs separately
    // in ensureStarted — the two are distinct events by design (§4.5.4).
    log("plugin loaded: '%s' v%s (kernel %s, abi %u)",
        rec->meta.name.c_str(), rec->meta.version.c_str(),
        CCORDIS_VERSION, unsigned(CCORDIS_ABI_VERSION));
    ensureStarted(*rec);
    if (rec->state == State::Deferred)
        log("plugin deferred: '%s' v%s (requirements not yet met)",
            rec->meta.name.c_str(), rec->meta.version.c_str());
}

void Context::plugin(const std::string &name, const PluginMeta &meta,
                     const ApplyFn &apply, const Value &config)
{
    LoadRecord *rec = new LoadRecord;
    rec->meta = meta;
    rec->meta.name = name;
    rec->apply = apply;
    rec->config = config;
    addRecord(rec);
}

void Context::plugin(const std::string &name, IPlugin *instance,
                     const Value &config, const std::vector<std::string> &required)
{
    if (!instance)
        return;
    LoadRecord *rec = new LoadRecord;
    rec->meta = PluginMeta{name, name, required, {}};
    // Object form: bind the pre-built instance into every activation; the
    // caller keeps ownership (cordis object plugins belong to their module).
    rec->apply = [rec, instance](Context &scope, const Value &cfg) {
        rec->objectInstance = instance;
        rec->callDispose = true;
        instance->apply(scope, cfg);
    };
    rec->config = config;
    addRecord(rec);
}

bool Context::pluginFromLibrary(const std::string &path, const Value &config)
{
    std::unique_ptr<SharedLibrary> lib(new SharedLibrary);
    if (!lib->load(path)) {
        log("cannot load plugin library '%s': %s",
            path.c_str(), lib->errorString().c_str());
        return false;
    }
    typedef const PluginDef *(*EntryFn)();
    EntryFn entry = reinterpret_cast<EntryFn>(lib->resolve("ccordis_plugin_entry"));
    if (!entry) {
        log("plugin library '%s' does not export ccordis_plugin_entry()",
            path.c_str());
        return false;   // lib dtor unloads an unresolved candidate
    }
    PluginDef *def = const_cast<PluginDef *>(entry());
    if (def->abiVersion != CCORDIS_ABI_VERSION) {                  // G1 握手
        log("plugin library '%s' abiVersion=%u does not match kernel %u "
            "(rebuild the plugin against current ccordis headers)",
            path.c_str(), unsigned(def->abiVersion), unsigned(CCORDIS_ABI_VERSION));
        return false;
    }

    LoadRecord *rec = new LoadRecord;
    rec->meta = metaFromDef(def);
    rec->def = def;
    rec->library = std::move(lib);
    rec->config = config;
    rec->apply = [rec](Context &scope, const Value &cfg) {
        rec->objectInstance = rec->def->create();
        rec->callDispose = true;
        rec->objectInstance->apply(scope, cfg);
    };
    addRecord(rec);
    return true;
}

bool Context::pluginFromDef(const PluginDef *def, const Value &config)
{
    if (!def || !def->create || !def->destroy)
        return false;
    if (def->abiVersion != CCORDIS_ABI_VERSION) {                  // G1 握手
        log("adopted plugin '%s' abiVersion=%u does not match kernel %u",
            def->name ? def->name : "?", unsigned(def->abiVersion),
            unsigned(CCORDIS_ABI_VERSION));
        return false;
    }
    PluginDef *d = const_cast<PluginDef *>(def);

    LoadRecord *rec = new LoadRecord;
    rec->meta = metaFromDef(d);
    rec->def = d;
    rec->config = config;      // adopted: library pointer stays null → the
    rec->apply = [rec](Context &scope, const Value &cfg) {  // loader unloads
        rec->objectInstance = rec->def->create();
        rec->callDispose = true;
        rec->objectInstance->apply(scope, cfg);
    };
    addRecord(rec);
    return true;
}

bool Context::unload(const std::string &name)
{
    for (auto it = m_records.begin(); it != m_records.end(); ++it) {
        if ((*it)->meta.name != name)
            continue;
        LoadRecord &rec = **it;
        if (rec.state == State::Active || rec.state == State::Failed)
            stopRecord(rec, State::Disposed);
        registry().unwatchAll(rec.depWatchTokens);
        if (rec.library)
            m_retiredLibraries.push_back(std::move(rec.library)); // §7.3
        m_records.erase(it);   // unique_ptr drops the record
        return true;
    }
    return false;
}

Context::State Context::pluginState(const std::string &name) const
{
    for (const auto &rec : m_records) {
        if (rec->meta.name == name)
            return rec->state;
    }
    return State::Disposed;    // unknown == not loaded in this scope
}

std::string Context::pluginError(const std::string &name) const
{
    for (const auto &rec : m_records) {
        if (rec->meta.name == name)
            return rec->lastError;
    }
    return std::string();
}

std::string Context::pluginVersion(const std::string &name) const
{
    for (const auto &rec : m_records) {
        if (rec->meta.name == name)
            return rec->meta.version;
    }
    return "0.0.0";   // unknown == not loaded in this scope / not declared
}

// ── config-driven bootstrap ─────────────────────────────────────────────────

void Context::registerFactory(const std::string &name, const PluginMeta &meta,
                              const ApplyFn &apply)
{
    m_factories[name] = std::make_pair(meta, apply);
}

bool Context::loadRegistered(const std::string &name, const Value &config)
{
    auto it = m_factories.find(name);
    if (it == m_factories.end()) {
        log("no registered plugin factory named '%s'", name.c_str());
        return false;
    }
    plugin(name, it->second.first, it->second.second, config);
    return true;
}

void Context::loadConfig(const Value &doc)
{
    const Value::Array *entries = nullptr;
    Value holder;   // keeps at("plugins") alive: asArray() must never point
                    // into a temporary (dangling-pointer hazard)
    if (const Value::Array *arr = doc.asArray()) {
        entries = arr;                          // bare array form
    } else {
        holder = doc.at("plugins");
        entries = holder.asArray();             // { "plugins": [...] } form
    }
    if (!entries)
        return;
    for (const Value &entry : *entries) {
        const std::string name = entry.at("plugin").toString();
        const Value options = entry.at("options");
        if (name.empty())
            continue;
        if (name.rfind("lib:", 0) == 0)
            pluginFromLibrary(name.substr(4), options);
        else
            loadRegistered(name, options);
    }
}

// ── events / middleware / data plane ────────────────────────────────────────

void Context::on(const std::string &event, const EventBus::Handler &handler)
{
    // Scope-bound: the connection lives in m_connections and releases
    // automatically when this scope dies (manual control: bus().on()).
    m_connections.push_back(bus().on(event, handler));
}

void Context::emitEvent(const std::string &event, const EventBus::Payload &payload)
{
    bus().emitEvent(event, payload);
}

void Context::watchAny(const std::string &name, const ServiceRegistry::Watcher &cb)
{
    std::weak_ptr<bool> alive = m_alive;
    m_watchTokens.push_back(registry().watch(
        name, [alive, cb](const std::shared_ptr<void> &erased) {
            if (alive.lock())
                cb(erased);
        }));
}

void Context::middleware(const Middleware &mw, bool prepend)
{
    if (prepend)
        m_middleware.insert(m_middleware.begin(), mw);
    else
        m_middleware.push_back(mw);
}

void Context::run(EventBus::Payload &payload)
{
    // Snapshot: middleware may be installed/uninstalled while running.
    const std::vector<Middleware> chain = m_middleware;
    std::function<void(std::size_t)> step = [&](std::size_t idx) {
        if (idx >= chain.size())
            return;
        chain[idx](payload, [&, idx]() { step(idx + 1); });
    };
    step(0);
}

std::shared_ptr<BlobChannel> Context::channel(const std::string &name)
{
    auto &map = *m_channels;
    auto it = map.find(name);
    if (it != map.end())
        return it->second;
    auto ch = std::make_shared<BlobChannel>();
    map[name] = ch;
    return ch;
}

void Context::onBlob(const std::string &name, const BlobChannel::Handler &h)
{
    m_blobSubs.push_back(channel(name)->subscribe(h));
}

void Context::pumpAll()
{
    for (auto &pair : *m_channels)
        pair.second->pump();
}

BlobPool &Context::blobPool()
{
    // Root-owned: walk up so every scope in the tree shares one pool
    // (a per-scope pool would fragment recycling across plugins).
    Context *root = this;
    while (root->m_parent)
        root = root->m_parent;
    if (!root->m_blobPool)
        root->m_blobPool = std::make_shared<BlobPool>();
    return *root->m_blobPool;
}

// ── introspection ───────────────────────────────────────────────────────────

Value Context::topology() const
{
    Value::Array plugins;
    for (const auto &rec : m_records) {
        Value::Object node;
        node.push_back({"name", Value(rec->meta.name)});
        if (!rec->meta.label.empty())
            node.push_back({"label", Value(rec->meta.label)});
        node.push_back({"version", Value(rec->meta.version)});
        node.push_back({"state", Value(stateToString(rec->state))});
        Value::Array req;
        for (const std::string &r : rec->meta.required)
            req.push_back(Value(r));
        node.push_back({"requires", Value(req)});
        if (!rec->lastError.empty())
            node.push_back({"error", Value(rec->lastError)});
        if (rec->scope) {
            Value::Array prov;
            for (const std::string &s :
                 registry().serviceNamesForOwner(rec->scope.get()))
                prov.push_back(Value(s));
            node.push_back({"provides", Value(prov)});
            node.push_back({"children",
                            rec->scope->topology().at("plugins")});
        }
        plugins.push_back(Value(node));
    }
    return Value::object({{"plugins", Value(plugins)}});
}

// ── lifecycle machinery ─────────────────────────────────────────────────────

Context::LoadRecord *Context::currentRecord()
{
    return m_startingRecord;
}

Context::LoadRecord *Context::findRecord(const std::string &name)
{
    for (auto &rec : m_records) {
        if (rec->meta.name == name)
            return rec.get();
    }
    return nullptr;
}

bool Context::requiresMet(const std::vector<std::string> &required) const
{
    for (const std::string &r : required) {
        if (!registry().available(r))
            return false;
    }
    return true;
}

void Context::armDependencyWatchers(LoadRecord &rec)
{
    if (rec.meta.required.empty())
        return;
    LoadRecord *recPtr = &rec;
    std::weak_ptr<bool> alive = m_alive;
    for (const std::string &dep : rec.meta.required) {
        rec.depWatchTokens.push_back(registry().watch(
            dep, [alive, this, recPtr](const std::shared_ptr<void> &service) {
                if (!alive.lock())
                    return;
                if (service) {
                    if (recPtr->state == State::Deferred)
                        ensureStarted(*recPtr);
                } else {
                    if (recPtr->state == State::Active)
                        stopRecord(*recPtr, State::Deferred); // dep went away
                }
            }));
    }
}

void Context::ensureStarted(LoadRecord &rec)
{
    if (rec.state != State::Deferred || !rec.apply)
        return;
    if (!requiresMet(rec.meta.required))
        return;                                 // stays deferred

    rec.state = State::Active;                  // set first: re-entrancy guard
    rec.scope = std::shared_ptr<Context>(new Context(this));
    m_startingRecord = &rec;
    try {
        rec.apply(*rec.scope, rec.config);
    } catch (const std::exception &e) {
        rec.lastError = e.what();
        log("plugin '%s' threw: %s", rec.meta.name.c_str(), e.what());
        stopRecord(rec, State::Failed);
    } catch (...) {
        rec.lastError = "unknown exception";
        log("plugin '%s' threw an unknown exception", rec.meta.name.c_str());
        stopRecord(rec, State::Failed);
    }
    m_startingRecord = nullptr;
    // Activation log: fires on EVERY start — including dependency-restart
    // re-entries via the Deferred machinery — because each is a real
    // lifecycle event an operator wants in the log (§4.5.4).
    if (rec.state == State::Active)
        log("plugin activated: '%s' v%s (kernel %s, abi %u)",
            rec.meta.name.c_str(), rec.meta.version.c_str(),
            CCORDIS_VERSION, unsigned(CCORDIS_ABI_VERSION));
}

void Context::stopRecord(LoadRecord &rec, State target)
{
    if (rec.state != State::Active && rec.state != State::Failed)
        return;
    rec.state = target;                         // set first: re-entrancy guard

    // 1) Tear down the child scope: revokes its services, which cascades to
    //    downstream plugins watching them (they re-defer) — a cordis
    //    dependency change rippling through the graph.
    rec.scope.reset();

    // 2) Release activation-owned resources: dispose hook first (never let
    //    it skip the destroy below), then instances.
    if (rec.callDispose && rec.objectInstance) {
        try {
            rec.objectInstance->dispose();
        } catch (const std::exception &e) {
            log("plugin '%s' dispose threw: %s", rec.meta.name.c_str(), e.what());
        } catch (...) {
            log("plugin '%s' dispose threw an unknown exception",
                rec.meta.name.c_str());
        }
    }
    if (rec.def && rec.objectInstance)
        rec.def->destroy(rec.objectInstance);   // dynamic: plugin-side delete
    rec.objectInstance = nullptr;               // object form: caller keeps it
    rec.owned.clear();                          // class-form instances die
}

} // namespace ccordis
