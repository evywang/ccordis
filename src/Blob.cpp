#include <ccordis/Blob.h>

#include <atomic>
#include <deque>
#include <mutex>
#include <new>
#include <vector>

namespace ccordis {

// ── Blob ───────────────────────────────────────────────────────────────────

Blob Blob::wrap(const void *data, std::size_t size,
                std::shared_ptr<const void> owner)
{
    Blob b;
    b.m_data = static_cast<const std::uint8_t *>(data);
    b.m_size = size;
    b.m_keepAlive = std::move(owner);
    return b;
}

Blob Blob::allocate(std::size_t size, BlobPool &pool)
{
    std::shared_ptr<std::uint8_t> block = pool.acquire(size);
    // Read the pointer BEFORE moving ownership into the keep-alive token —
    // parameter initialization order is unspecified and moving first would
    // null the source shared_ptr.
    const std::uint8_t *p = block.get();
    return wrap(p, size, std::shared_ptr<const void>(std::move(block)));
}

// ── BlobPool ───────────────────────────────────────────────────────────────

namespace {

constexpr std::size_t kMinBucketExp = 6;          // 64 B minimum bucket

std::size_t bucketExpFor(std::size_t size)
{
    std::size_t exp = kMinBucketExp;
    while ((std::size_t(1) << exp) < size && exp < 63)
        ++exp;
    return exp;
}

} // namespace

/**
 * @brief Shared internal state — outlives the public pool handle.
 *
 * Every block deleter captures a shared_ptr to this state, so a Blob that
 * escapes its pool's destruction parks/frees against *live* state (dead
 * flag) instead of touching freed memory. The public BlobPool is a thin
 * handle; State is the single source of truth under its own mutex.
 */
struct BlobPool::State
{
    struct Parked {
        std::uint8_t *ptr;
        std::size_t capacity;
    };

    const std::size_t alignment;
    const std::size_t maxRetained;
    std::mutex mutex;
    std::vector<std::deque<Parked>> freelist;    // per power-of-two bucket
    std::size_t retainedBytes = 0;
    std::uint64_t allocations = 0;
    std::uint64_t reuseHits = 0;
    bool dead = false;

    State(std::size_t alignBytes, std::size_t maxRetainedBytes)
        : alignment(alignBytes), maxRetained(maxRetainedBytes)
    {
        freelist.resize(64 - kMinBucketExp + 1);
    }

    /** Park a released block if the pool is alive and within budget.
     *  @return false when the caller must plain-free the block instead. */
    bool park(std::uint8_t *p, std::size_t capacity)
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (dead || retainedBytes + capacity > maxRetained)
            return false;
        const std::size_t idx = bucketExpFor(capacity) - kMinBucketExp;
        freelist[idx].push_back(Parked{p, capacity});
        retainedBytes += capacity;
        return true;
    }
};

BlobPool::BlobPool(std::size_t maxRetainedBytes, std::size_t alignment)
    : m_state(new State(alignment, maxRetainedBytes))
{
}

BlobPool::~BlobPool()
{
    // Free every parked block now; late Blob releases see dead=true and
    // plain-free their own memory. State itself stays alive for whichever
    // deleters still reference it.
    std::lock_guard<std::mutex> lock(m_state->mutex);
    for (auto &bucket : m_state->freelist) {
        for (State::Parked &pb : bucket)
            ::operator delete(pb.ptr, std::align_val_t(m_state->alignment));
        bucket.clear();
    }
    m_state->retainedBytes = 0;
    m_state->dead = true;
}

std::shared_ptr<std::uint8_t> BlobPool::acquire(std::size_t size)
{
    if (size == 0)
        size = 1;
    const std::size_t exp = bucketExpFor(size);
    const std::size_t capacity = std::size_t(1) << exp;
    const std::size_t idx = exp - kMinBucketExp;

    // Fast path: pop a parked block of this bucket, re-wrap it with the
    // recycling deleter.
    std::uint8_t *raw = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        auto &bucket = m_state->freelist[idx];
        if (!bucket.empty()) {
            raw = bucket.front().ptr;
            bucket.pop_front();
            m_state->retainedBytes -= capacity;
            ++m_state->reuseHits;
        }
    }

    // Slow path: heap-allocate a SIMD-aligned block.
    if (!raw) {
        raw = static_cast<std::uint8_t *>(
            ::operator new(capacity, std::align_val_t(m_state->alignment)));
        std::lock_guard<std::mutex> lock(m_state->mutex);
        ++m_state->allocations;
    }

    std::shared_ptr<State> st = m_state;
    return std::shared_ptr<std::uint8_t>(raw, [st, capacity](std::uint8_t *p) {
        if (!st->park(p, capacity))
            ::operator delete(p, std::align_val_t(st->alignment));
    });
}

BlobPool::Stats BlobPool::stats() const
{
    std::lock_guard<std::mutex> lock(m_state->mutex);
    Stats s;
    s.allocations = m_state->allocations;
    s.reuseHits = m_state->reuseHits;
    s.retainedBytes = m_state->retainedBytes;
    std::size_t blocks = 0;
    for (const auto &bucket : m_state->freelist)
        blocks += bucket.size();
    s.retainedBlocks = blocks;
    return s;
}

} // namespace ccordis
