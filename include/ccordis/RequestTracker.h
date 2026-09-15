#ifndef CCORDIS_REQUESTTRACKER_H
#define CCORDIS_REQUESTTRACKER_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <vector>

namespace ccordis {

/**
 * @brief Tick-driven in-flight request tracker — the timeout half of the
 *        command path (design doc §9.2: requestId 关联 + 超时管理).
 *
 * Deliberately a *pure data structure*: no OS timer, no thread, no callback
 * into user code — the host drives time via tick(now) from whatever beat it
 * already has (Qt QTimer, IO heartbeat, main-loop poll). This keeps the
 * dependency red line (§0) intact while giving the command facade a
 * deterministic, host-thread-only expiry mechanism.
 *
 * Lifecycle of a request id:
 *
 *   track(id, timeout) ──► Pending ──complete()──► Acked
 *                          │  ▲
 *                 tick() 到期│  │ 迟到响应
 *                          ▼  │
 *                      TimedOut ──complete()──► LateAck (有界历史内)
 *   未曾 track/已出历史 ──────────complete()──► Unknown
 *
 * Policy (retry / fail / UI feedback) lives in the command facade, which
 * translates tick()'s expired-id list into cmd.timeout events and resends.
 */
class RequestTracker
{
public:
    using Clock = std::chrono::steady_clock;
    using Id = std::uint64_t;

    enum class Outcome { Acked, LateAck, Unknown };

    struct Stats {
        std::uint64_t inFlight = 0;
        std::uint64_t acked = 0;
        std::uint64_t timedOut = 0;
        std::uint64_t lateAcks = 0;   // responses that arrived after timeout
    };

    /** Register an in-flight request with its per-request timeout.
     *  Time is injectable for the same reason tick() takes it: the host (and
     *  tests) drive one coherent clock. Re-tracking a live id refreshes its
     *  deadline. */
    void track(Id id, Clock::duration timeout,
               Clock::time_point now = Clock::now());

    /** Record a response for `id`.
     *  - Acked:    was in flight — normal completion, caller proceeds
     *  - LateAck:  had already timed out — log/reconcile, do NOT touch UI
     *  - Unknown:  never tracked (e.g. stale after expireAll) — ignore */
    Outcome complete(Id id);

    /** Advance time; returns ids whose deadline elapsed, in deadline order.
     *  The facade turns these into cmd.timeout events / retry decisions. */
    std::vector<Id> tick(Clock::time_point now);

    /** Fail-fast on connection loss: everything in flight expires at once.
     *  Returns the expired ids (in deadline order). */
    std::vector<Id> expireAll();

    /** Forget an id without classifying it (used when a retry re-registers
     *  under a new id and the old ack should map to the operation instead). */
    void forget(Id id);

    std::size_t inFlightCount() const { return m_inFlight.size(); }
    Stats stats() const;

private:
    void moveTimedOut(const std::vector<Id> &ids);

    struct Entry {
        Clock::time_point deadline;
    };
    std::map<Id, Entry> m_inFlight;                       // id -> deadline
    std::map<Clock::time_point, std::vector<Id>> m_wheel; // deadline -> ids

    // Bounded LRU of recently-timed-out ids so late acks are classifiable.
    static constexpr std::size_t kHistory = 256;
    std::deque<Id> m_timedOutHistory;

    Stats m_stats;
};

} // namespace ccordis

#endif // CCORDIS_REQUESTTRACKER_H
