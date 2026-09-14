#ifndef CCORDIS_BLOBCHANNEL_H
#define CCORDIS_BLOBCHANNEL_H

#include "Blob.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace ccordis {

/**
 * @brief Named data-plane channel — fast fan-out of large Blob objects
 *        between plugins (control plane is EventBus; this is the data plane).
 *
 * Threading contract (deliberately asymmetric):
 *   - subscribe() / publish() / pump()   → host thread only
 *   - post()                             → any thread (device threads)
 *
 * Delivery paths:
 *   - publish(blob): synchronous snapshot fan-out on the calling (host)
 *     thread — N subscribers each receive the same Blob (pointer identity,
 *     zero byte copies; one refcount bump per subscriber).
 *   - post(blob): any-thread enqueue into a bounded mailbox; overflow drops
 *     the OLDEST item (latest-wins — right default for live streams like
 *     spectrum frames). The host drains with pump() on its event loop.
 *   - A pump hook (setPumpHook) fires on empty→nonempty transitions so a
 *     host can schedule one drain (e.g. a Qt timer) and stay idle otherwise.
 *
 * Subscriptions are RAII: destroying the returned ScopedSubscription
 * detaches the handler (auto-released also when the owning scope dies —
 * the Context-level ctx.onBlob() binding does exactly that).
 */
class BlobChannel
{
public:
    using Handler = std::function<void(const Blob &)>;
    using Token = std::uint64_t;

    struct Stats {
        std::uint64_t published = 0;  // synchronous publishes
        std::uint64_t posted = 0;     // queued posts accepted
        std::uint64_t dropped = 0;    // posts dropped (overflow, oldest)
        std::uint64_t pumped = 0;     // queued posts delivered
        std::uint64_t delivered = 0;  // handler invocations (all paths)
    };

    /** RAII subscription handle — detach on destruction. */
    class ScopedSubscription
    {
    public:
        explicit ScopedSubscription(BlobChannel *ch = nullptr, Token t = 0)
            : m_channel(ch), m_token(t) {}
        ~ScopedSubscription() { release(); }
        ScopedSubscription(const ScopedSubscription &) = delete;
        ScopedSubscription &operator=(const ScopedSubscription &) = delete;
        ScopedSubscription(ScopedSubscription &&o) noexcept
            : m_channel(o.m_channel), m_token(o.m_token)
        {
            o.m_channel = nullptr;
            o.m_token = 0;
        }
        ScopedSubscription &operator=(ScopedSubscription &&o) noexcept
        {
            if (this != &o) {
                release();
                m_channel = o.m_channel;
                m_token = o.m_token;
                o.m_channel = nullptr;
                o.m_token = 0;
            }
            return *this;
        }
        void release()
        {
            if (m_channel && m_token) {
                m_channel->unsubscribe(m_token);
                m_channel = nullptr;
                m_token = 0;
            }
        }
    private:
        BlobChannel *m_channel;
        Token m_token;
    };

    /**
     * @param capacity mailbox depth for post()/pump() (default 8).
     */
    explicit BlobChannel(std::size_t capacity = 8);

    /** Host thread. Handler invoked synchronously on publish/pump. */
    ScopedSubscription subscribe(const Handler &handler);

    /** Host thread. Synchronous fan-out to current subscribers. */
    void publish(const Blob &blob);

    /** Any thread. Bounded enqueue; overflow drops oldest. */
    void post(Blob blob);

    /** Host thread. Drain the mailbox in FIFO order via publish(). */
    void pump();

    /**
     * @brief Wake-up hook, invoked ON THE POSTING THREAD when the mailbox
     *        transitions empty→nonempty (i.e. typically a device thread).
     *
     * Contract (design review A1/F1): the hook must only perform a cheap,
     * thread-safe wake-up scheduling — e.g. QMetaObject::invokeMethod(...,
     * QueuedConnection), writing an eventfd/pipe, or posting a sem_post.
     * It must NEVER touch host-thread-affine objects directly (QTimer,
     * widgets): timers and GUI objects belong to the host thread.
     * Re-armed after each pump drain.
     */
    void setPumpHook(std::function<void()> hook) { m_pumpHook = std::move(hook); }

    void setCapacity(std::size_t capacity);
    Stats stats() const;
    std::size_t queueDepth() const;

private:
    friend class ScopedSubscription;
    void unsubscribe(Token token);
    std::vector<Handler> snapshotHandlers();

    mutable std::mutex m_handlerMutex;          // guards m_handlers
    std::map<Token, Handler> m_handlers;
    Token m_nextToken = 1;

    mutable std::mutex m_queueMutex;            // guards m_queue/m_pending
    std::deque<Blob> m_queue;
    std::size_t m_capacity;
    bool m_pending = false;                     // hook re-arm flag
    std::function<void()> m_pumpHook;           // producer-thread wake-up

    // Any-thread counters (post() may run on device threads).
    mutable std::atomic<std::uint64_t> m_published{0};
    mutable std::atomic<std::uint64_t> m_posted{0};
    mutable std::atomic<std::uint64_t> m_dropped{0};
    mutable std::atomic<std::uint64_t> m_pumped{0};
    mutable std::atomic<std::uint64_t> m_delivered{0};
};

} // namespace ccordis

#endif // CCORDIS_BLOBCHANNEL_H
