/**
 * @file MemoryPool.h
 * @brief FastNet memory-pool and buffer-pool helpers
 *
 * Key correctness fixes vs previous version:
 *  - BufferPool deleter no longer accesses thread_local from an arbitrary
 *    thread; instead it routes freed Buffers back through a lock-free
 *    per-pool return queue that the owning thread drains on next alloc.
 *  - MemoryPool::allocate() now returns nullptr on OOM instead of
 *    propagating std::bad_alloc through the call stack.
 *  - ThreadCache lookup avoids the per-call hash table search by
 *    embedding the cache directly as a thread_local inside the template
 *    instantiation, keyed by a static per-instance token.
 */
#pragma once

#include "Config.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <utility>
#include <vector>

namespace FastNet {

struct MemoryPoolStats {
    size_t allocated = 0;
    size_t free = 0;
    size_t total = 0;
};

template<size_t BlockSize = 4096>
class MemoryPool {
public:
    explicit MemoryPool(size_t preAllocateCount = 64) {
        warmUp(preAllocateCount);
    }

    ~MemoryPool() {
        // Drain any pending cross-thread returns first, then the TLS cache.
        drainReturnQueue();
        releaseLocalCache();
    }

    MemoryPool(const MemoryPool&) = delete;
    MemoryPool& operator=(const MemoryPool&) = delete;

    void* allocate() noexcept {
        // Drain any blocks returned from other threads before checking local cache.
        drainReturnQueue();

        // Fast path: take from thread-local cache without any locking.
        auto& cache = threadCache();
        if (cache.head != nullptr) {
            Block* block = cache.head;
            cache.head = block->next;
            block->next = nullptr;
            --cache.count;
            cachedCount_.fetch_sub(1, std::memory_order_relaxed);
            allocatedCount_.fetch_add(1, std::memory_order_relaxed);
            return static_cast<void*>(block->storage);
        }

        // Slow path: batch-refill from global pool under lock.
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (freeList_ == nullptr) {
                const size_t currentTotal =
                    allocatedCount_.load(std::memory_order_relaxed) + freeCount_;
                const size_t growCount = currentTotal == 0 ? 32 : (currentTotal / 2) + 1;
                // Guard against OOM: return nullptr instead of propagating bad_alloc.
                try {
                    expandPoolUnlocked(growCount);
                } catch (const std::bad_alloc&) {
                    return nullptr;
                }
            }

            // Take up to kLocalCacheRefill blocks into thread-local cache.
            constexpr size_t kLocalCacheRefill = 16;
            size_t taken = 0;
            while (freeList_ != nullptr && taken < kLocalCacheRefill) {
                Block* block = freeList_;
                freeList_ = block->next;
                block->next = cache.head;
                cache.head = block;
                ++cache.count;
                --freeCount_;
                cachedCount_.fetch_add(1, std::memory_order_relaxed);
                ++taken;
            }
        }

        if (cache.head == nullptr) {
            return nullptr;
        }
        Block* block = cache.head;
        cache.head = block->next;
        block->next = nullptr;
        --cache.count;
        cachedCount_.fetch_sub(1, std::memory_order_relaxed);
        allocatedCount_.fetch_add(1, std::memory_order_relaxed);
        return static_cast<void*>(block->storage);
    }

    void deallocate(void* ptr) noexcept {
        if (ptr == nullptr) {
            return;
        }

        static_assert(offsetof(Block, storage) == 0,
                      "Block::storage must be the first member for safe container_of");
        Block* block = reinterpret_cast<Block*>(static_cast<uint8_t*>(ptr));
        allocatedCount_.fetch_sub(1, std::memory_order_relaxed);

        // Fast path: return to thread-local cache (same thread as alloc, common case).
        auto& cache = threadCache();
        block->next = cache.head;
        cache.head = block;
        ++cache.count;
        cachedCount_.fetch_add(1, std::memory_order_relaxed);

        // If the local cache overflows, drain half back to global pool.
        constexpr size_t kLocalCacheDrainThreshold = 64;
        if (cache.count > kLocalCacheDrainThreshold) {
            const size_t drainCount = cache.count / 2;
            Block* drainHead = nullptr;
            Block* drainTail = nullptr;
            for (size_t i = 0; i < drainCount; ++i) {
                Block* b = cache.head;
                cache.head = b->next;
                --cache.count;
                b->next = drainHead;
                if (drainTail == nullptr) {
                    drainTail = b;
                }
                drainHead = b;
            }
            std::lock_guard<std::mutex> lock(mutex_);
            drainTail->next = freeList_;
            freeList_ = drainHead;
            freeCount_ += drainCount;
            cachedCount_.fetch_sub(drainCount, std::memory_order_relaxed);
        }
    }

    // Cross-thread deallocation: push the block into the lock-free return queue.
    // The owning allocator thread drains this queue on next allocate().
    void deallocateFromOtherThread(void* ptr) noexcept {
        if (ptr == nullptr) {
            return;
        }
        static_assert(offsetof(Block, storage) == 0,
                      "Block::storage must be the first member for safe container_of");
        Block* block = reinterpret_cast<Block*>(static_cast<uint8_t*>(ptr));
        allocatedCount_.fetch_sub(1, std::memory_order_relaxed);

        // Push onto the return queue using a lock-free Treiber stack.
        Block* oldHead = returnQueue_.load(std::memory_order_relaxed);
        do {
            block->next = oldHead;
        } while (!returnQueue_.compare_exchange_weak(
            oldHead, block,
            std::memory_order_release,
            std::memory_order_relaxed));
    }

    void warmUp(size_t count) {
        if (count == 0) {
            return;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        try {
            expandPoolUnlocked(count);
        } catch (const std::bad_alloc&) {
            // Warm-up is best-effort; ignore OOM here.
        }
    }

    size_t getAllocatedCount() const {
        return getStats().allocated;
    }

    size_t getFreeCount() const {
        return getStats().free;
    }

    size_t getTotalCount() const {
        return getStats().total;
    }

    MemoryPoolStats getStats() const {
        std::lock_guard<std::mutex> lock(mutex_);
        const size_t allocated = allocatedCount_.load(std::memory_order_relaxed);
        const size_t cached = cachedCount_.load(std::memory_order_relaxed);
        return MemoryPoolStats{allocated, freeCount_ + cached, allocated + freeCount_ + cached};
    }

private:
    struct Block {
        alignas(std::max_align_t) std::uint8_t storage[BlockSize];
        Block* next = nullptr;
    };

    struct ThreadCache {
        Block* head = nullptr;
        size_t count = 0;
    };

    // ── Thread-local cache — O(1) lookup via per-instance token ─────────────
    //
    // Each MemoryPool instance gets a unique token (monotonically-increasing
    // index) from a static atomic counter.  thread_local storage is a flat
    // vector indexed by that token, giving O(1) access with no hash computation
    // on the hot alloc/dealloc path (vs. the old unordered_map approach).

    // Assign a unique token to this pool instance at construction time.
    const size_t instanceToken_ = nextToken();

    static size_t nextToken() noexcept {
        static std::atomic<size_t> counter{0};
        return counter.fetch_add(1, std::memory_order_relaxed);
    }

    static std::vector<ThreadCache>& threadCacheVec() {
        thread_local std::vector<ThreadCache> vec;
        return vec;
    }

    ThreadCache& threadCache() {
        auto& vec = threadCacheVec();
        if (instanceToken_ >= vec.size()) {
            vec.resize(instanceToken_ + 1);
        }
        return vec[instanceToken_];
    }

    void releaseLocalCache() noexcept {
        auto& vec = threadCacheVec();
        if (instanceToken_ >= vec.size()) {
            return;  // This thread never touched this pool
        }
        ThreadCache& cache = vec[instanceToken_];
        if (cache.head == nullptr) {
            return;
        }
        // Walk to the tail once to prepend the whole chain to freeList_.
        Block* head = cache.head;
        Block* tail = head;
        size_t count = 1;
        while (tail->next != nullptr) {
            tail = tail->next;
            ++count;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            tail->next = freeList_;
            freeList_ = head;
            freeCount_ += count;
        }
        cachedCount_.fetch_sub(cache.count, std::memory_order_relaxed);
        cache.head  = nullptr;
        cache.count = 0;
    }

    // Drain the lock-free return queue (cross-thread deallocations) into the
    // global free list under mutex_.
    //
    // IMPORTANT: We intentionally do NOT write to threadCache() here.
    //
    // Original design: drain into the caller's TLS cache.  This hits a subtle
    // lifecycle bug: if the allocating thread's TLS (~ThreadCache) has already
    // been destroyed before the pool destructor runs, and a still-alive thread
    // calls drainReturnQueue() — which previously wrote into the callers' TLS —
    // the write lands in a potentially-dead TLS object (UB, possible crash on
    // MSVC / glibc TLS teardown orderings).
    //
    // Safe design: always append to freeList_ under the global mutex_.
    // Cost: one mutex acquire per drain (drain is low-frequency — triggered only
    // at allocate() time), so the extra contention is negligible.
    void drainReturnQueue() noexcept {
        Block* list = returnQueue_.exchange(nullptr, std::memory_order_acquire);
        if (list == nullptr) {
            return;
        }
        // Walk to the tail of the returned chain, count nodes.
        Block* tail = list;
        size_t count = 1;
        while (tail->next != nullptr) {
            tail = tail->next;
            ++count;
        }
        // Prepend the entire chain to freeList_ under the global lock.
        {
            std::lock_guard<std::mutex> lock(mutex_);
            tail->next = freeList_;
            freeList_ = list;
            freeCount_ += count;
        }
        // Do NOT touch cachedCount_ here — these blocks are now in freeList_,
        // not in any TLS cache, so cachedCount_ should not be adjusted.
        // allocatedCount_ was already decremented in deallocateFromOtherThread.
    }

    void expandPoolUnlocked(size_t count) {
        // Throws std::bad_alloc on OOM — callers must catch.
        auto chunk = std::make_unique<Block[]>(count);
        for (size_t index = 0; index < count; ++index) {
            chunk[index].next = freeList_;
            freeList_ = &chunk[index];
            ++freeCount_;
        }
        chunks_.push_back(std::move(chunk));
    }

    mutable std::mutex mutex_;
    Block* freeList_ = nullptr;
    // Lock-free return queue for cross-thread deallocations (Treiber stack).
    std::atomic<Block*> returnQueue_{nullptr};
    std::vector<std::unique_ptr<Block[]>> chunks_;
    std::atomic<size_t> allocatedCount_{0};
    std::atomic<size_t> cachedCount_{0};
    size_t freeCount_ = 0;
};

struct BufferPoolStats {
    size_t smallAllocated = 0;
    size_t smallFree = 0;
    size_t largeAllocated = 0;
    size_t largeFree = 0;
};

class FASTNET_API BufferPool {
public:
    static BufferPool& getInstance() {
        static BufferPool instance;
        return instance;
    }

    std::shared_ptr<Buffer> allocateBuffer(size_t size = 8192) {
        return allocateReservedBuffer(size, size);
    }

    // NOTE: This function returns a shared_ptr whose custom deleter is
    // guaranteed to be thread-safe.  When the last owner releases the
    // shared_ptr (possibly on a *different* thread from the allocating
    // thread), the deleter enqueues the Buffer into an atomic MPSC return
    // queue instead of writing to thread-local storage.  The allocating
    // thread drains this queue on the next call to allocateBuffer().
    std::shared_ptr<Buffer> allocateReservedBuffer(size_t reserveSize, size_t initialSize = 0) {
        if (initialSize > reserveSize) {
            reserveSize = initialSize;
        }

        // Drain any Buffers returned from other threads.
        drainReturnQueue();

        Buffer* buffer = nullptr;
        const bool isSmall = reserveSize <= kSmallReserveSize;
        const bool isLarge = !isSmall && reserveSize <= kLargeReserveSize;

        auto& cache = localCache();

        if (isSmall && !cache.smallBuffers.empty()) {
            buffer = cache.smallBuffers.back();
            cache.smallBuffers.pop_back();
        } else if (isLarge && !cache.largeBuffers.empty()) {
            buffer = cache.largeBuffers.back();
            cache.largeBuffers.pop_back();
        } else {
            buffer = new (std::nothrow) Buffer();
            if (!buffer) {
                return nullptr;
            }
        }

        try {
            buffer->reserve(reserveSize);
            buffer->resize(initialSize);
        } catch (...) {
            delete buffer;
            throw;
        }

        // Capture raw pool pointer so the deleter can enqueue without
        // accessing thread_local from the releasing thread.
        BufferPool* const pool = this;
        return std::shared_ptr<Buffer>(buffer, [pool](Buffer* ptr) noexcept {
            ptr->clear();
            pool->returnBuffer(ptr);
        });
    }

    void warmUp(size_t smallCount, size_t largeCount) {
        auto& cache = localCache();
        for (size_t i = 0; i < smallCount && cache.smallBuffers.size() < 128; ++i) {
            auto* b = new (std::nothrow) Buffer();
            if (!b) break;
            try {
                b->reserve(kSmallReserveSize);
            } catch (...) {
                delete b;
                break;
            }
            cache.smallBuffers.push_back(b);
        }
        for (size_t i = 0; i < largeCount && cache.largeBuffers.size() < 64; ++i) {
            auto* b = new (std::nothrow) Buffer();
            if (!b) break;
            try {
                b->reserve(kLargeReserveSize);
            } catch (...) {
                delete b;
                break;
            }
            cache.largeBuffers.push_back(b);
        }
    }

    BufferPoolStats getStats() const {
        auto& cache = localCache();
        return BufferPoolStats{0, cache.smallBuffers.size(), 0, cache.largeBuffers.size()};
    }

    void getStats(size_t& smallAllocated,
                  size_t& smallFree,
                  size_t& largeAllocated,
                  size_t& largeFree) const {
        const auto stats = getStats();
        smallAllocated = stats.smallAllocated;
        smallFree      = stats.smallFree;
        largeAllocated = stats.largeAllocated;
        largeFree      = stats.largeFree;
    }

private:
    static constexpr size_t kSmallReserveSize = 8192;
    static constexpr size_t kLargeReserveSize = 65536;

    // Internal node for the lock-free return queue (Treiber stack).
    struct ReturnNode {
        Buffer*     buffer = nullptr;
        ReturnNode* next   = nullptr;
    };

    BufferPool() = default;
    ~BufferPool() {
        // Drain any pending cross-thread returns and delete everything.
        drainReturnQueue();
        auto& cache = localCache();
        for (auto* b : cache.smallBuffers) delete b;
        for (auto* b : cache.largeBuffers) delete b;
        cache.smallBuffers.clear();
        cache.largeBuffers.clear();
    }

    BufferPool(const BufferPool&) = delete;
    BufferPool& operator=(const BufferPool&) = delete;

    // Called by the deleter — may run on ANY thread.
    void returnBuffer(Buffer* ptr) noexcept {
        // Allocate a ReturnNode on the heap.  This is a one-time cost per
        // cross-thread return; nodes themselves are pooled in a side-channel
        // for zero-alloc steady-state (future optimization).
        ReturnNode* node = new (std::nothrow) ReturnNode{ptr, nullptr};
        if (!node) {
            // Absolute last resort: free directly rather than touching another
            // thread's TLS cache.
            delete ptr;
            return;
        }
        // Push onto the Treiber stack.
        ReturnNode* oldHead = returnQueue_.load(std::memory_order_relaxed);
        do {
            node->next = oldHead;
        } while (!returnQueue_.compare_exchange_weak(
            oldHead, node,
            std::memory_order_release,
            std::memory_order_relaxed));
    }

    // Drain the cross-thread return queue into the local TLS cache.
    // Must be called from the allocating thread.
    void drainReturnQueue() noexcept {
        ReturnNode* list = returnQueue_.exchange(nullptr, std::memory_order_acquire);
        if (!list) return;
        auto& cache = localCache();
        while (list) {
            ReturnNode* next = list->next;
            Buffer* buf = list->buffer;
            delete list;
            list = next;
            if (!buf) continue;
            if (buf->capacity() <= kSmallReserveSize && cache.smallBuffers.size() < 128) {
                cache.smallBuffers.push_back(buf);
            } else if (buf->capacity() <= kLargeReserveSize && cache.largeBuffers.size() < 64) {
                cache.largeBuffers.push_back(buf);
            } else {
                delete buf;
            }
        }
    }

    struct Cache {
        std::vector<Buffer*> smallBuffers;
        std::vector<Buffer*> largeBuffers;
        ~Cache() {
            for (auto* b : smallBuffers) delete b;
            for (auto* b : largeBuffers) delete b;
        }
    };

    static Cache& localCache() {
        thread_local Cache cache;
        return cache;
    }

    // Lock-free MPSC return queue (Treiber stack).  Writers are any thread;
    // the sole reader is the allocating thread inside drainReturnQueue().
    std::atomic<ReturnNode*> returnQueue_{nullptr};
};

} // namespace FastNet
