#ifndef CCORDIS_BLOB_H
#define CCORDIS_BLOB_H

#include <cstddef>
#include <cstdint>
#include <memory>

namespace ccordis {

/**
 * @brief Immutable byte view with shared keep-alive — the unit of the data
 *        plane (large binary transfer between plugins).
 *
 * A Blob never owns logic of its own: it is a (data, size) view plus an
 * optional shared ownership token. Copying a Blob copies two pointers and
 * bumps one refcount; publishing to N subscribers is N atomic increments
 * and zero byte copies.
 *
 * Construction modes:
 *   - Blob::allocate(n, pool): pooled block (SIMD-aligned, recycled);
 *   - Blob::wrap(p, n, owner): zero-copy wrap of *external* memory (e.g. a
 *     device DMA buffer or a std::vector owned elsewhere) — pass a
 *     shared_ptr as `owner` to keep it alive for consumers.
 *
 * Immutability is by contract: after a Blob has been published, no writer
 * may touch its bytes (single-writer completes before publish).
 */
class Blob
{
public:
    Blob() = default;

    /** Wrap external memory. `owner` may be null when the caller
     *  guarantees the memory outlives all consumers. */
    static Blob wrap(const void *data, std::size_t size,
                     std::shared_ptr<const void> owner = {});

    /** Allocate a pooled, cache-line-aligned block of exactly `size` bytes.
     *  The block returns to the pool when the last Blob referencing it dies
     *  (or is plain-freed if the pool itself is gone). */
    static Blob allocate(std::size_t size, class BlobPool &pool);

    const std::uint8_t *data() const { return m_data; }
    std::size_t size() const { return m_size; }
    bool valid() const { return m_data != nullptr; }
    explicit operator bool() const { return valid(); }

private:
    const std::uint8_t *m_data = nullptr;
    std::size_t m_size = 0;
    std::shared_ptr<const void> m_keepAlive;   // holds the block alive
};

/**
 * @brief Recycling allocator for Blob blocks — steady-state zero-allocation.
 *
 * - Blocks are aligned (default 64B, cache line / AVX friendly) and sized
 *   by power-of-two buckets: requesting 900B reuses a freed 1024B block.
 * - Retention is bounded (maxRetainedBytes, default 64 MiB): released
 *   blocks beyond the budget are returned to the heap immediately.
 * - Thread-safe: producers on device threads may acquire blocks directly
 *   (fill off-thread, then post — see BlobChannel).
 * - Deleter safety: blocks that outlive the pool are plain-freed via the
 *   pool's liveness token; they never touch dead pool state.
 */
class BlobPool
{
public:
    struct Stats {
        std::uint64_t allocations = 0;   // heap allocations performed
        std::uint64_t reuseHits = 0;     // satisfied from freelist
        std::size_t retainedBlocks = 0;  // blocks currently parked
        std::size_t retainedBytes = 0;   // bytes currently parked
    };

    /**
     * @param maxRetainedBytes upper bound on parked (freed-but-kept) memory.
     * @param alignment block alignment; power of two, ≥ alignof(max_align_t).
     */
    explicit BlobPool(std::size_t maxRetainedBytes = 64u << 20,
                      std::size_t alignment = 64);
    ~BlobPool();

    BlobPool(const BlobPool &) = delete;
    BlobPool &operator=(const BlobPool &) = delete;

    /** Acquire a block usable for `size` bytes (capacity rounds up to the
     *  bucket). Returns a shared_ptr whose deleter recycles the block. */
    std::shared_ptr<std::uint8_t> acquire(std::size_t size);

    Stats stats() const;

private:
    struct State;                               // shared internals (Blob.cpp)
    std::shared_ptr<State> m_state;             // outlives the pool handle
};

} // namespace ccordis

#endif // CCORDIS_BLOB_H
