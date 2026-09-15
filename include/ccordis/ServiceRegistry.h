#ifndef CCORDIS_SERVICEREGISTRY_H
#define CCORDIS_SERVICEREGISTRY_H

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <typeindex>
#include <utility>
#include <vector>

namespace ccordis {

class Context;

/**
 * @brief Typed service registry — the C++ counterpart of cordis
 *        provide/inject and of the OSGi service registry cordis descends
 *        from (cf. CppMicroServices, Apache-2.0). Design §4.3.
 *
 * A service is keyed by (concrete type, string id) and stored type-erased
 * as shared_ptr<void> alongside its owning Context for scope teardown.
 *
 * Late binding: watchers fire with the current state immediately on
 * registration (nullptr = absent), then on every appear/disappear
 * transition — the data face of the plugin defer/restart state machine.
 *
 * Threading: host thread; the mutex protects teardown re-entrancy only.
 */
class ServiceRegistry
{
public:
    using Watcher = std::function<void(const std::shared_ptr<void> &)>;
    using WatchToken = std::uint64_t;

    struct ServiceEntry {
        std::string name;                // dotted service id
        std::type_index type;
        std::shared_ptr<void> instance;
        const void *owner;               // owning Context, for teardown
    };

    /** Register (or replace-in-place for the same owner) a service. */
    void provide(const std::string &name, const std::type_info &type,
                 const std::shared_ptr<void> &instance, const void *owner);

    /** Remove the service `name` owned by `owner` (scope teardown only). */
    void revoke(const std::string &name, const void *owner);

    /** Lookup with strict type check; nullptr when absent or mismatched. */
    std::shared_ptr<void> inject(const std::string &name,
                                 const std::type_info &type) const;

    bool available(const std::string &name) const;

    std::vector<std::string> serviceNames() const;
    std::vector<std::string> serviceNamesForOwner(const void *owner) const;

    /** Watch a service id (immediate state delivery + transitions).
     *  Returns the token used to detach via unwatch(). */
    WatchToken watch(const std::string &name, const Watcher &watcher);
    void unwatch(WatchToken token);
    void unwatchAll(const std::vector<WatchToken> &tokens);

private:
    void fireWatchers(const std::string &name);

    mutable std::mutex m_mutex;
    std::map<std::string, ServiceEntry> m_services;
    std::map<WatchToken, std::pair<std::string, Watcher>> m_watchers;
    WatchToken m_nextWatchToken = 1;
};

/**
 * @brief Typed convenience adapter used by plugin code via Context.
 * The registry already type-checked; the static cast is safe by contract.
 */
template <typename T>
struct ServiceCast {
    static std::shared_ptr<T> from(const std::shared_ptr<void> &erased)
    {
        return std::static_pointer_cast<T>(erased);
    }
};

} // namespace ccordis

#endif // CCORDIS_SERVICEREGISTRY_H
