#include "EventBus.h"
#include "Log.h"

namespace ccordis {

EventBus::ScopedConnection EventBus::on(const std::string &event,
                                        const Handler &handler)
{
    if (!handler)
        return ScopedConnection();
    std::lock_guard<std::mutex> lock(m_mutex);
    const Token token = m_nextToken++;
    m_handlers[event].push_back(Entry{token, handler});
    return ScopedConnection(this, token);
}

void EventBus::emitEvent(const std::string &event, const Payload &payload)
{
    // Snapshot under the lock, dispatch outside it: handlers may emit
    // re-entrantly or unsubscribe (release grabs the same mutex).
    std::vector<Handler> snapshot;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto it = m_handlers.find(event);
        if (it == m_handlers.end())
            return;
        snapshot.reserve(it->second.size());
        for (const Entry &e : it->second)
            snapshot.push_back(e.handler);
    }
    for (const Handler &h : snapshot) {
        try {
            h(payload);
        } catch (const std::exception &e) {
            log("event '%s' handler threw: %s", event.c_str(), e.what());
        } catch (...) {
            log("event '%s' handler threw an unknown exception", event.c_str());
        }
    }
}

void EventBus::off(Token token)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto &pair : m_handlers) {
        std::vector<Entry> &entries = pair.second;
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (entries[i].token == token) {
                entries.erase(entries.begin() + static_cast<long>(i));
                return;
            }
        }
    }
}

} // namespace ccordis
