/**
 * @file EventPoller.h
 * @brief FastNet readiness-based event poller
 *
 * v1.5 changes:
 *  - eventMap_ replaced by FlatHashMap for cache-friendly O(1) lookup.
 *  - EventCallback stored as shared_ptr<const EventCallback> inside EventData so
 *    that poll() dispatch copies only an atomic refcount pointer, not the entire
 *    std::function object.
 *  - EPOLLONESHOT support (Linux): safe concurrent epoll_wait from multiple
 *    threads; re-arm via rearm() after each event dispatch.
 *  - add() / modify() / remove() still protected by shared_mutex (readers are
 *    cheap: just shared_lock to copy the shared_ptr).
 */
#pragma once

#include "Config.h"
#include "FlatHashMap.h"
#include "SocketWrapper.h"

#include <atomic>
#include <functional>
#include <memory>
#include <shared_mutex>
#include <type_traits>

namespace FastNet {

// ── Event type flags ──────────────────────────────────────────────────────────

enum class EventType {
    None  = 0,
    Read  = 1 << 0,
    Write = 1 << 1,
    Error = 1 << 2,
    Close = 1 << 3,
};

inline EventType operator| (EventType l, EventType r) {
    using U = std::underlying_type_t<EventType>;
    return static_cast<EventType>(static_cast<U>(l) | static_cast<U>(r));
}
inline EventType operator& (EventType l, EventType r) {
    using U = std::underlying_type_t<EventType>;
    return static_cast<EventType>(static_cast<U>(l) & static_cast<U>(r));
}
inline EventType& operator|=(EventType& l, EventType r) { l = l | r; return l; }
inline bool hasEvent(EventType events, EventType check) {
    using U = std::underlying_type_t<EventType>;
    return (static_cast<U>(events) & static_cast<U>(check)) != 0;
}

// ── Callback types ────────────────────────────────────────────────────────────

using EventCallback = std::function<void(socket_t fd, EventType events, void* data, size_t len)>;

// ── EventData ─────────────────────────────────────────────────────────────────

/**
 * Per-fd registration data stored in eventMap_.
 *
 * The callback is wrapped in a shared_ptr so that copying it in poll() costs
 * only one atomic refcount increment, not a std::function copy-constructor
 * (which may allocate if the captured closure exceeds SBO size).
 */
struct EventData {
    socket_t                          fd       = INVALID_SOCKET_FD;
    EventType                         events   = EventType::None;
    std::shared_ptr<EventCallback>    callback;   // shared_ptr: cheap to copy
    void*                             userData = nullptr;
};

// ── EventPoller ───────────────────────────────────────────────────────────────

class FASTNET_API EventPoller {
public:
    EventPoller() noexcept;
    ~EventPoller() noexcept;

    EventPoller(const EventPoller&)            = delete;
    EventPoller& operator=(const EventPoller&) = delete;

    // ── Lifecycle ─────────────────────────────────────────────────────────

    /**
     * Initialize with @p maxEvents slots.
     * On Linux, pass useOneShot=true to register all fds with EPOLLONESHOT,
     * enabling safe concurrent epoll_wait from multiple threads (no duplicate
     * dispatch for the same fd).  After poll() dispatches an event, you MUST
     * call rearm() to re-enable that fd.
     */
    bool initialize(size_t maxEvents = 1024, bool useOneShot = false);
    void shutdown() noexcept;

    // ── Registration ──────────────────────────────────────────────────────

    bool add(socket_t fd,
             EventType events,
             const EventCallback& callback,
             void* userData = nullptr) noexcept;

    bool modify(socket_t fd, EventType events) noexcept;

    bool remove(socket_t fd) noexcept;

    /**
     * Re-arm a one-shot fd after its event has been dispatched.
     * Only relevant when useOneShot=true was passed to initialize().
     * No-op on Windows (IOCP handles this automatically).
     */
    bool rearm(socket_t fd, EventType events) noexcept;

    // ── Dispatch ──────────────────────────────────────────────────────────

    /**
     * Wait up to @p timeoutMs ms for ready events and dispatch callbacks.
     * Returns the number of callbacks invoked, or -1 on fatal error.
     * Thread-safe: may be called concurrently when useOneShot=true (Linux).
     */
    int poll(int timeoutMs = -1) noexcept;

    /**
     * Interrupt a blocking poll() call from another thread.
     */
    bool wakeup() noexcept;

    // ── Status ────────────────────────────────────────────────────────────

    bool   isRunning()      const noexcept { return running_.load(std::memory_order_acquire); }
    size_t getSocketCount() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;

    // FlatHashMap replaces unordered_map:
    //   - contiguous flat storage → better cache locality on lookup
    //   - Robin Hood probing → bounded probe-sequence length
    //   - No heap node per entry
    FlatHashMap<socket_t, EventData> eventMap_;

    mutable std::shared_mutex mutex_;
    std::atomic<bool>         running_{false};
    bool                      oneShot_ = false;
};

} // namespace FastNet
