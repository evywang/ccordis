#ifndef CCORDIS_EVENTBUS_H
#define CCORDIS_EVENTBUS_H

#include <ccordis/Value.h>

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace ccordis {

/**
 * @brief Synchronous named-event bus — the control plane, C++ counterpart
 *        of cordis `ctx.on/ctx.emit` (design §4.2).
 *
 * Semantics:
 *  - Payload is a ccordis::Value, conventionally an object.
 *  - Ordering: handlers of one event fire in SUBSCRIPTION order (FIFO);
 *    this is a promised guarantee, not an implementation accident.
 *  - Exceptions: handlers must not throw; the kernel catches per handler
 *    at the dispatch boundary and logs, so one bad handler never breaks
 *    the rest of the fan-out or unwinds through teardown (review A3/F3).
 *  - Re-entrancy: emitEvent snapshots the handler table; handlers may
 *    subscribe/unsubscribe/re-emit during dispatch.
 *  - Storm guard (gap G4): emit depth is capped (kMaxRecurse, default 16).
 *    A handler chain that re-emits beyond the cap is dropped with a log —
 *    a naive A→handler→emit e2→handler→emit e1 cycle would otherwise
 *    recurse until the host thread stack overflows (exception isolation
 *    cannot help: nothing throws).
 *
 * Threading: host thread only (single-thread contract, design §12);
 * the mutex exists solely for snapshot consistency and teardown re-entrancy.
 */
class EventBus
{
public:
    using Payload = Value;
    using Handler = std::function<void(const Payload &)>;
    using Token = std::uint64_t;

    /** RAII subscription handle. Unsubscribes on destruction. */
    class ScopedConnection
    {
    public:
        explicit ScopedConnection(EventBus *bus = nullptr, Token token = 0)
            : m_bus(bus), m_token(token) {}
        ~ScopedConnection() { release(); }

        ScopedConnection(const ScopedConnection &) = delete;
        ScopedConnection &operator=(const ScopedConnection &) = delete;

        ScopedConnection(ScopedConnection &&other) noexcept
            : m_bus(other.m_bus), m_token(other.m_token)
        {
            other.m_bus = nullptr;
            other.m_token = 0;
        }
        ScopedConnection &operator=(ScopedConnection &&other) noexcept
        {
            if (this != &other) {
                release();
                m_bus = other.m_bus;
                m_token = other.m_token;
                other.m_bus = nullptr;
                other.m_token = 0;
            }
            return *this;
        }

        void release()
        {
            if (m_bus && m_token) {
                m_bus->off(m_token);
                m_bus = nullptr;
                m_token = 0;
            }
        }

        bool valid() const { return m_bus != nullptr && m_token != 0; }

    private:
        EventBus *m_bus;
        Token m_token;
    };

    /** Subscribe to `event`; returns the RAII handle owning the slot. */
    ScopedConnection on(const std::string &event, const Handler &handler);

    /**
     * @brief Fire `event` synchronously to all current subscribers, in
     *        subscription order. Named emitEvent: Qt hosts own the `emit`
     *        keyword-macro and kernel headers must survive inclusion
     *        after Qt headers.
     */
    void emitEvent(const std::string &event, const Payload &payload = Payload());

private:
    friend class ScopedConnection;
    void off(Token token);

    struct Entry {
        Token token;
        Handler handler;
    };

    std::mutex m_mutex;                          // guards m_handlers
    std::map<std::string, std::vector<Entry>> m_handlers; // append = FIFO
    Token m_nextToken = 1;
    unsigned m_depth = 0;                        // re-entrancy depth (G4)
    static constexpr unsigned kMaxRecurse = 16;
};

} // namespace ccordis

#endif // CCORDIS_EVENTBUS_H
