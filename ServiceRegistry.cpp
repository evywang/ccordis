#include "ServiceRegistry.h"
#include "Log.h"

namespace ccordis {

void ServiceRegistry::provide(const std::string &name,
                              const std::type_info &type,
                              const std::shared_ptr<void> &instance,
                              const void *owner)
{
    if (!instance)
        return;
    bool appeared = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        // ServiceEntry has no default ctor (std::type_index); find +
        // insert/assign explicitly — map::operator[] is unusable here.
        auto it = m_services.find(name);
        if (it == m_services.end()) {
            m_services.insert(std::make_pair(
                name, ServiceEntry{name, type, instance, owner}));
            appeared = true;
        } else if (it->second.owner == owner) {
            // Same provider re-provides (dependency restart): replace in
            // place; watchers only fire on appear/disappear transitions.
            it->second.type = type;
            it->second.instance = instance;
        }
        // A different owner providing an existing id is a configuration
        // error; the first provider wins (single-owner-per-id contract).
    }
    if (appeared)
        fireWatchers(name);
}

void ServiceRegistry::revoke(const std::string &name, const void *owner)
{
    bool disappeared = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_services.find(name);
        if (it != m_services.end() && it->second.owner == owner) {
            m_services.erase(it);
            disappeared = true;
        }
    }
    if (disappeared)
        fireWatchers(name);
}

std::shared_ptr<void> ServiceRegistry::inject(const std::string &name,
                                              const std::type_info &type) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_services.find(name);
    if (it == m_services.end() || it->second.type != type)
        return nullptr;
    return it->second.instance;
}

bool ServiceRegistry::available(const std::string &name) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_services.find(name) != m_services.end();
}

std::vector<std::string> ServiceRegistry::serviceNames() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<std::string> names;
    names.reserve(m_services.size());
    for (const auto &pair : m_services)
        names.push_back(pair.first);
    return names;
}

std::vector<std::string> ServiceRegistry::serviceNamesForOwner(
    const void *owner) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<std::string> names;
    for (const auto &pair : m_services) {
        if (pair.second.owner == owner)
            names.push_back(pair.first);
    }
    return names;
}

ServiceRegistry::WatchToken ServiceRegistry::watch(const std::string &name,
                                                   const Watcher &watcher)
{
    if (!watcher)
        return 0;
    std::shared_ptr<void> current;
    WatchToken token = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        token = m_nextWatchToken++;
        m_watchers[token] = std::make_pair(name, watcher);
        auto it = m_services.find(name);
        if (it != m_services.end())
            current = it->second.instance;
    }
    try {
        watcher(current);   // immediate state delivery (nullptr = absent)
    } catch (const std::exception &e) {
        log("watcher on '%s' threw: %s", name.c_str(), e.what());
    } catch (...) {
        log("watcher on '%s' threw an unknown exception", name.c_str());
    }
    return token;
}

void ServiceRegistry::unwatch(WatchToken token)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_watchers.erase(token);
}

void ServiceRegistry::unwatchAll(const std::vector<WatchToken> &tokens)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (WatchToken token : tokens)
        m_watchers.erase(token);
}

void ServiceRegistry::fireWatchers(const std::string &name)
{
    // Snapshot matching watchers under the lock; a watcher may itself call
    // provide/revoke/watch (re-entrancy), which takes the same mutex.
    std::vector<Watcher> toFire;
    std::shared_ptr<void> current;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_services.find(name);
        if (it != m_services.end())
            current = it->second.instance;
        for (const auto &pair : m_watchers) {
            if (pair.second.first == name)
                toFire.push_back(pair.second.second);
        }
    }
    for (const Watcher &w : toFire) {
        try {
            w(current);
        } catch (const std::exception &e) {
            log("watcher on '%s' threw: %s", name.c_str(), e.what());
        } catch (...) {
            log("watcher on '%s' threw an unknown exception", name.c_str());
        }
    }
}

} // namespace ccordis
