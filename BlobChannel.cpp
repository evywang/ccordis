#include "BlobChannel.h"

namespace ccordis {

BlobChannel::BlobChannel(std::size_t capacity)
    : m_capacity(capacity ? capacity : 1)
{
}

BlobChannel::ScopedSubscription BlobChannel::subscribe(const Handler &handler)
{
    if (!handler)
        return ScopedSubscription();
    std::lock_guard<std::mutex> lock(m_handlerMutex);
    const Token token = m_nextToken++;
    m_handlers[token] = handler;
    return ScopedSubscription(this, token);
}

void BlobChannel::unsubscribe(Token token)
{
    std::lock_guard<std::mutex> lock(m_handlerMutex);
    m_handlers.erase(token);
}

std::vector<BlobChannel::Handler> BlobChannel::snapshotHandlers()
{
    // Snapshot under the lock, invoke outside: handlers may subscribe or
    // unsubscribe during dispatch without deadlocking or invalidating.
    std::lock_guard<std::mutex> lock(m_handlerMutex);
    std::vector<Handler> snapshot;
    snapshot.reserve(m_handlers.size());
    for (const auto &pair : m_handlers)
        snapshot.push_back(pair.second);
    return snapshot;
}

void BlobChannel::publish(const Blob &blob)
{
    ++m_published;
    const std::vector<Handler> handlers = snapshotHandlers();
    for (const Handler &h : handlers) {
        h(blob);
        ++m_delivered;
    }
}

void BlobChannel::post(Blob blob)
{
    bool fireHook = false;
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        if (m_queue.size() >= m_capacity) {
            m_queue.pop_front();                // drop oldest (latest-wins)
            ++m_dropped;
        }
        m_queue.push_back(std::move(blob));
        ++m_posted;
        if (!m_pending) {
            m_pending = true;
            fireHook = true;                    // empty→nonempty transition
        }
    }
    if (fireHook && m_pumpHook)
        m_pumpHook();                           // host schedules one pump
}

void BlobChannel::pump()
{
    // Swap the mailbox out under the lock; posts arriving while we deliver
    // re-arm the hook for the next pump (no loss, no double drain).
    std::deque<Blob> drained;
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        drained.swap(m_queue);
        m_pending = false;
    }
    for (const Blob &blob : drained) {
        ++m_pumped;
        publish(blob);
    }
}

void BlobChannel::setCapacity(std::size_t capacity)
{
    std::lock_guard<std::mutex> lock(m_queueMutex);
    m_capacity = capacity ? capacity : 1;
    while (m_queue.size() > m_capacity) {
        m_queue.pop_front();
        ++m_dropped;
    }
}

BlobChannel::Stats BlobChannel::stats() const
{
    Stats s;
    s.published = m_published.load(std::memory_order_relaxed);
    s.posted = m_posted.load(std::memory_order_relaxed);
    s.dropped = m_dropped.load(std::memory_order_relaxed);
    s.pumped = m_pumped.load(std::memory_order_relaxed);
    s.delivered = m_delivered.load(std::memory_order_relaxed);
    return s;
}

std::size_t BlobChannel::queueDepth() const
{
    std::lock_guard<std::mutex> lock(m_queueMutex);
    return m_queue.size();
}

} // namespace ccordis
