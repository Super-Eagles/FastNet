/**
 * @file SpinLock.h
 * @brief Adaptive spinlock with TTAS + exponential back-off and OS yield fallback
 *
 * Suitable for short critical sections (< ~500 ns) where avoiding kernel
 * context switches outweighs the cost of spinning.  Falls back to
 * std::this_thread::yield() when the back-off budget is exhausted, so it
 * won't starve other threads on single-core machines or under heavy load.
 *
 * Key design choices vs. a naïve test_and_set loop:
 *  1. "Test-then-test-and-set" (TTAS): spin on a read-only atomic<bool> load
 *     first so the cache line stays in Shared state.  Only issue the expensive
 *     RMW (test_and_set on atomic_flag) once the lock *looks* free.  This
 *     dramatically reduces coherence traffic on NUMA / multi-socket systems.
 *  2. Exponential back-off capped at 64 pause instructions per outer iteration.
 *     On Xeon/EPYC class CPUs 256× pause saturates the memory subsystem under
 *     heavy contention; 64 is a better empirical sweet-spot.
 *  3. After kMaxSpinCount hard spins we yield to the OS scheduler, so we never
 *     starve other threads indefinitely.
 *  4. Fully C++17 compatible: uses atomic<bool> for the read-only test (no
 *     C++20 atomic_flag::test() required).
 *
 * Satisfies BasicLockable and Lockable, so it works with std::lock_guard,
 * std::unique_lock, and std::scoped_lock.
 */
#pragma once

#include <atomic>
#include <thread>

#if defined(_MSC_VER)
#include <intrin.h>
#define FASTNET_CPU_PAUSE() _mm_pause()
#elif defined(__x86_64__) || defined(__i386__)
#define FASTNET_CPU_PAUSE() __asm__ volatile("pause" ::: "memory")
#elif defined(__aarch64__) || defined(__arm__)
#define FASTNET_CPU_PAUSE() __asm__ volatile("yield" ::: "memory")
#else
#define FASTNET_CPU_PAUSE() std::atomic_thread_fence(std::memory_order_seq_cst)
#endif

namespace FastNet {

class SpinLock {
public:
    SpinLock() noexcept = default;

    SpinLock(const SpinLock&)            = delete;
    SpinLock& operator=(const SpinLock&) = delete;

    void lock() noexcept {
        // Phase 1: TTAS (test-then-test-and-set) with exponential back-off.
        //
        // We spin on `locked_` (atomic<bool>) with a plain load which keeps
        // the cache line in Shared state.  Only attempt test_and_set when the
        // lock appears free, minimising bus-invalidate traffic.
        for (int spin = 0; spin < kMaxSpinCount; ++spin) {
            if (tryAcquire()) {
                return;
            }
            // Exponential pause: 2^min(spin,6) pause instructions per iteration
            // = at most 64 pauses, avoiding memory-bus saturation on NUMA.
            const int pauses = 1 << (spin < 6 ? spin : 6);
            for (int p = 0; p < pauses; ++p) {
                FASTNET_CPU_PAUSE();
            }
            // Read-only spin while locked — no RMW, no Exclusive cache-line.
            while (locked_.load(std::memory_order_relaxed)) {
                FASTNET_CPU_PAUSE();
            }
        }
        // Phase 2: yield to the OS scheduler to avoid indefinite starvation.
        while (!tryAcquire()) {
            std::this_thread::yield();
        }
    }

    bool try_lock() noexcept {
        return tryAcquire();
    }

    void unlock() noexcept {
        locked_.store(false, std::memory_order_release);
    }

private:
    bool tryAcquire() noexcept {
        // Fast path: read-only check first (stays Shared), then RMW only
        // when the lock appears free.  Both operations are C++17-compatible.
        return !locked_.load(std::memory_order_relaxed) &&
               !locked_.exchange(true, std::memory_order_acquire);
    }

    // Max outer spin iterations before falling back to OS yield.
    // 64 is empirically better than 256 on high-core-count NUMA systems.
    static constexpr int kMaxSpinCount = 64;

    // atomic<bool> instead of atomic_flag: allows the read-only load in the
    // TTAS inner loop (atomic_flag::test() requires C++20).
    std::atomic<bool> locked_{false};
};

// RAII guard wrapping SpinLock (like std::lock_guard but for SpinLock).
class SpinLockGuard {
public:
    explicit SpinLockGuard(SpinLock& lock) noexcept : lock_(lock) { lock_.lock(); }
    ~SpinLockGuard() noexcept { lock_.unlock(); }

    SpinLockGuard(const SpinLockGuard&)            = delete;
    SpinLockGuard& operator=(const SpinLockGuard&) = delete;

private:
    SpinLock& lock_;
};

} // namespace FastNet
