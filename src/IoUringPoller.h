/**
 * @file IoUringPoller.h
 * @brief Linux io_uring-based event poller backend (requires Linux >= 5.1)
 *
 * Enabled by defining FASTNET_ENABLE_IOURING at compile time.
 * Falls back gracefully to the epoll backend if io_uring is unavailable
 * at runtime.
 *
 * Key advantages over epoll:
 *  - Fully async accept/read/write: zero system-call overhead per operation.
 *  - Batching via Submission Queue (SQ): multiple ops submitted in one syscall.
 *  - Completion Queue (CQ) polling can run without any syscall at all (SQPOLL).
 *  - sendmsg/recvmsg scatter-gather natively in the queue.
 */
#pragma once

#ifdef __linux__
#ifdef FASTNET_ENABLE_IOURING

#include "Config.h"
#include "Error.h"
#include "SocketWrapper.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

// liburing header (install via: apt install liburing-dev / vcpkg install liburing)
#include <liburing.h>

namespace FastNet {

// Callback type for io_uring completions.
// cqe_res  = io_uring_cqe::res  (bytes transferred or negative errno)
// user_data = user_data tag associated with the SQE
using IoUringCompletionFn = std::function<void(int32_t cqeRes, uintptr_t userData)>;

enum class IoUringOpType : uint8_t {
    Accept,
    Recv,
    Send,
    Sendmsg,    // scatter-gather send
    Recvmsg,    // scatter-gather recv
    Close,
    Cancel,
};

struct IoUringOp {
    IoUringOpType        type;
    socket_t             fd;
    uintptr_t            userData;   // echoed back in the completion
    IoUringCompletionFn  callback;   // called when CQE arrives
};

/**
 * IoUringPoller wraps a single io_uring instance.
 *
 * Usage pattern (mirrors EventPoller's interface):
 *
 *   IoUringPoller poller;
 *   if (!poller.initialize(maxEvents)) { // fall back to epoll }
 *   poller.submitAccept(listenFd, userData, callback);
 *   while (running) { poller.processCompletions(timeoutMs); }
 *   poller.shutdown();
 */
class FASTNET_API IoUringPoller {
public:
    IoUringPoller()  noexcept;
    ~IoUringPoller() noexcept;

    IoUringPoller(const IoUringPoller&)            = delete;
    IoUringPoller& operator=(const IoUringPoller&) = delete;

    // ── Lifecycle ─────────────────────────────────────────────────────────

    /**
     * Initialise the io_uring with @p sqDepth SQ/CQ entries.
     * Returns false if io_uring is not available (kernel < 5.1 or no CAP_SYS_ADMIN).
     * Caller should fall back to the epoll backend in that case.
     *
     * Flags:
     *   IORING_SETUP_SQPOLL  - kernel SQ polling thread (zero-syscall steady state)
     *                          Requires CAP_SYS_NICE or RLIMIT_NICE.
     */
    bool initialize(size_t sqDepth = 4096, bool enableSqPoll = false) noexcept;

    void shutdown() noexcept;

    bool isRunning() const noexcept { return initialized_; }

    // ── Submission helpers ────────────────────────────────────────────────

    /**
     * Submit an async accept.  When a new connection arrives the CQE's res
     * field contains the new client fd (>= 0) or a negative errno.
     */
    bool submitAccept(socket_t listenFd,
                      uintptr_t userData,
                      IoUringCompletionFn callback) noexcept;

    /**
     * Submit an async recv.  @p buffer must remain valid until the CQE fires.
     */
    bool submitRecv(socket_t fd,
                    void* buffer,
                    size_t len,
                    uintptr_t userData,
                    IoUringCompletionFn callback) noexcept;

    /**
     * Submit an async send.  @p data must remain valid until the CQE fires.
     */
    bool submitSend(socket_t fd,
                    const void* data,
                    size_t len,
                    uintptr_t userData,
                    IoUringCompletionFn callback) noexcept;

    /**
     * Submit a scatter-gather send via sendmsg.
     * @p msghdr must remain valid until the CQE fires.
     */
    bool submitSendmsg(socket_t fd,
                       const struct msghdr* msg,
                       uintptr_t userData,
                       IoUringCompletionFn callback) noexcept;

    /**
     * Submit a scatter-gather recv via recvmsg.
     * @p msghdr must remain valid until the CQE fires.
     */
    bool submitRecvmsg(socket_t fd,
                       struct msghdr* msg,
                       uintptr_t userData,
                       IoUringCompletionFn callback) noexcept;

    /**
     * Cancel a previously submitted operation by its user_data tag.
     */
    bool cancelOp(uintptr_t userData) noexcept;

    // ── Completion processing ─────────────────────────────────────────────

    /**
     * Wait up to @p timeoutMs ms and process all available CQEs.
     * Returns the number of completions processed, -1 on fatal error.
     *
     * With SQPOLL enabled, prefer timeoutMs = 0 (busy-polling) and call
     * this in a tight loop to achieve truly zero-sys-call IO.
     */
    int processCompletions(int timeoutMs = -1) noexcept;

    /**
     * Flush all pending SQEs to the kernel.
     * Normally called automatically; invoke if you want to force early submission.
     */
    int flush() noexcept;

    size_t inflight() const noexcept { return inflight_.load(std::memory_order_relaxed); }

private:
    // Internal SQE acquisition with automatic flush on ring full.
    io_uring_sqe* getSqe() noexcept;

    // Store the completion callback alongside the SQE's user_data.
    void registerCallback(uintptr_t userData, IoUringCompletionFn cb);
    IoUringCompletionFn takeCallback(uintptr_t userData);

    io_uring ring_{};
    bool     initialized_ = false;
    bool     sqPoll_      = false;

    std::atomic<size_t> inflight_{0};

    // Callback registry keyed by user_data.
    // In production code, replace with FlatHashMap<uintptr_t, ...>.
    struct CbEntry {
        uintptr_t           userData;
        IoUringCompletionFn callback;
    };
    std::vector<CbEntry> callbacks_;  // small vector; replace with flat_hash_map
    std::mutex           cbMutex_;

    // Pending accept sockaddr (kept alive for the duration of the SQE).
    struct AcceptState {
        sockaddr_storage addr{};
        socklen_t        addrLen = sizeof(sockaddr_storage);
    };
    std::vector<std::unique_ptr<AcceptState>> acceptStates_;
};

/**
 * Runtime probe: returns true if the running kernel supports io_uring
 * with at least the operations required by FastNet.
 */
FASTNET_API bool isIoUringAvailable() noexcept;

} // namespace FastNet

#endif  // FASTNET_ENABLE_IOURING
#endif  // __linux__
