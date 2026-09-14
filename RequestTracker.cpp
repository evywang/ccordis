#include "RequestTracker.h"

namespace ccordis {

void RequestTracker::track(Id id, Clock::duration timeout, Clock::time_point now)
{
    const Clock::time_point deadline = now + timeout;
    auto it = m_inFlight.find(id);
    if (it != m_inFlight.end()) {
        // Refresh: remove from the old deadline slot before re-arming.
        auto slot = m_wheel.find(it->second.deadline);
        if (slot != m_wheel.end()) {
            auto &v = slot->second;
            for (size_t i = 0; i < v.size(); ++i) {
                if (v[i] == id) {
                    v.erase(v.begin() + static_cast<long>(i));
                    break;
                }
            }
            if (v.empty())
                m_wheel.erase(slot);
        }
        it->second.deadline = deadline;
    } else {
        m_inFlight[id] = Entry{deadline};
    }
    m_wheel[deadline].push_back(id);
}

RequestTracker::Outcome RequestTracker::complete(Id id)
{
    auto it = m_inFlight.find(id);
    if (it != m_inFlight.end()) {
        auto slot = m_wheel.find(it->second.deadline);
        if (slot != m_wheel.end()) {
            auto &v = slot->second;
            for (size_t i = 0; i < v.size(); ++i) {
                if (v[i] == id) {
                    v.erase(v.begin() + static_cast<long>(i));
                    break;
                }
            }
            if (v.empty())
                m_wheel.erase(slot);
        }
        m_inFlight.erase(it);
        ++m_stats.acked;
        return Outcome::Acked;
    }
    // Late or unknown: consult the bounded history (single classification).
    for (size_t i = 0; i < m_timedOutHistory.size(); ++i) {
        if (m_timedOutHistory[i] == id) {
            m_timedOutHistory.erase(m_timedOutHistory.begin()
                                     + static_cast<long>(i));
            ++m_stats.lateAcks;
            return Outcome::LateAck;
        }
    }
    return Outcome::Unknown;
}

std::vector<RequestTracker::Id> RequestTracker::tick(Clock::time_point now)
{
    std::vector<Id> expired;
    while (!m_wheel.empty() && m_wheel.begin()->first <= now) {
        const std::vector<Id> ids = m_wheel.begin()->second;
        m_wheel.erase(m_wheel.begin());
        expired.insert(expired.end(), ids.begin(), ids.end());
    }
    moveTimedOut(expired);
    return expired;
}

std::vector<RequestTracker::Id> RequestTracker::expireAll()
{
    std::vector<Id> expired;
    for (const auto &slot : m_wheel)
        expired.insert(expired.end(), slot.second.begin(), slot.second.end());
    m_wheel.clear();
    moveTimedOut(expired);
    return expired;
}

void RequestTracker::moveTimedOut(const std::vector<Id> &ids)
{
    for (Id id : ids) {
        m_inFlight.erase(id);
        m_timedOutHistory.push_back(id);
        ++m_stats.timedOut;
    }
    while (m_timedOutHistory.size() > kHistory)
        m_timedOutHistory.pop_front();
}

void RequestTracker::forget(Id id)
{
    auto it = m_inFlight.find(id);
    if (it == m_inFlight.end())
        return;
    auto slot = m_wheel.find(it->second.deadline);
    if (slot != m_wheel.end()) {
        auto &v = slot->second;
        for (size_t i = 0; i < v.size(); ++i) {
            if (v[i] == id) {
                v.erase(v.begin() + static_cast<long>(i));
                break;
            }
        }
        if (v.empty())
            m_wheel.erase(slot);
    }
    m_inFlight.erase(it);
    // Not counted as acked or timed out — the operation continues under a
    // new id (retry); stats stay truthful about *outcomes*, not attempts.
}

RequestTracker::Stats RequestTracker::stats() const
{
    Stats s = m_stats;
    s.inFlight = m_inFlight.size();
    return s;
}

} // namespace ccordis
