/**
 * @file IoUringPoller.cpp
 * @brief Linux io_uring-based event poller implementation
 */
#ifdef __linux__
#ifdef FASTNET_ENABLE_IOURING

#include "IoUringPoller.h"

#include <cerrno>
#include <cstring>
#include <sys/utsname.h>

namespace FastNet {

// ── Runtime availability probe ──────────────────────────────────────────────

bool isIoUringAvailable() noexcept {
    // Probe 1: kernel version >= 5.1
    struct utsname uts{};
    if (::uname(&uts) == 0) {
        int major = 0, minor = 0;
        ::sscanf(uts.release, "%d.%d", &major, &minor);
        if (major < 5 || (major == 5 && minor < 1)) {
            return false;
        }
    }
    // Probe 2: attempt to create a minimal ring; destroy immediately.
    io_uring testRing{};
    const int ret = ::io_uring_queue_init(4, &testRing, 0);
    if (ret < 0) {
        return false;
    }
    ::io_uring_queue_exit(&testRing);
    return true;
}

// ── Construction / destruction ──────────────────────────────────────────────

IoUringPoller::IoUringPoller() noexcept = default;

IoUringPoller::~IoUringPoller() noexcept {
    shutdown();
}

// ── Lifecycle ───────────────────────────────────────────────────────────────

bool IoUringPoller::initialize(size_t sqDepth, bool enableSqPoll) noexcept {
    if (initialized_) {
        return true;
    }

    unsigned int flags = 0;
    if (enableSqPoll) {
        flags |= IORING_SETUP_SQPOLL;
        sqPoll_ = true;
    }

    // Round sqDepth up to next power of two (required by io_uring).
    if (sqDepth == 0) sqDepth = 4096;
    size_t pow2 = 1;
    while (pow2 < sqDepth) pow2 <<= 1;

    const int ret = ::io_uring_queue_init(static_cast<unsigned>(pow2), &ring_, flags);
    if (ret < 0) {
        return false;
    }

    initialized_ = true;
    return true;
}

void IoUringPoller::shutdown() noexcept {
    if (!initialized_) return;
    ::io_uring_queue_exit(&ring_);
    initialized_ = false;
    inflight_.store(0, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(cbMutex_);
    callbacks_.clear();
    acceptStates_.clear();
}

// ── Internal helpers ─────────────────────────────────────────────────────────

io_uring_sqe* IoUringPoller::getSqe() noexcept {
    io_uring_sqe* sqe = ::io_uring_get_sqe(&ring_);
    if (sqe == nullptr) {
        // Ring is full; flush to kernel and retry once.
        ::io_uring_submit(&ring_);
        sqe = ::io_uring_get_sqe(&ring_);
    }
    return sqe;
}

void IoUringPoller::registerCallback(uintptr_t userData, IoUringCompletionFn cb) {
    std::lock_guard<std::mutex> lock(cbMutex_);
    callbacks_.push_back({userData, std::move(cb)});
}

IoUringCompletionFn IoUringPoller::takeCallback(uintptr_t userData) {
    std::lock_guard<std::mutex> lock(cbMutex_);
    for (auto it = callbacks_.begin(); it != callbacks_.end(); ++it) {
        if (it->userData == userData) {
            IoUringCompletionFn cb = std::move(it->callback);
            callbacks_.erase(it);
            return cb;
        }
    }
    return {};
}

// ── Submission ───────────────────────────────────────────────────────────────

bool IoUringPoller::submitAccept(socket_t listenFd,
                                  uintptr_t userData,
                                  IoUringCompletionFn callback) noexcept {
    if (!initialized_) return false;
    io_uring_sqe* sqe = getSqe();
    if (!sqe) return false;

    // Keep the sockaddr alive for the life of the SQE.
    auto state = std::make_unique<AcceptState>();
    ::io_uring_prep_accept(sqe,
                           static_cast<int>(listenFd),
                           reinterpret_cast<sockaddr*>(&state->addr),
                           &state->addrLen,
                           SOCK_NONBLOCK | SOCK_CLOEXEC);
    ::io_uring_sqe_set_data64(sqe, static_cast<uint64_t>(userData));

    try {
        registerCallback(userData, std::move(callback));
        std::lock_guard<std::mutex> lock(cbMutex_);
        acceptStates_.push_back(std::move(state));
    } catch (...) {
        return false;
    }
    inflight_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool IoUringPoller::submitRecv(socket_t fd,
                                void* buffer,
                                size_t len,
                                uintptr_t userData,
                                IoUringCompletionFn callback) noexcept {
    if (!initialized_) return false;
    io_uring_sqe* sqe = getSqe();
    if (!sqe) return false;
    ::io_uring_prep_recv(sqe, static_cast<int>(fd), buffer, len, 0);
    ::io_uring_sqe_set_data64(sqe, static_cast<uint64_t>(userData));
    try { registerCallback(userData, std::move(callback)); } catch (...) { return false; }
    inflight_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool IoUringPoller::submitSend(socket_t fd,
                                const void* data,
                                size_t len,
                                uintptr_t userData,
                                IoUringCompletionFn callback) noexcept {
    if (!initialized_) return false;
    io_uring_sqe* sqe = getSqe();
    if (!sqe) return false;
    ::io_uring_prep_send(sqe, static_cast<int>(fd), data, len, MSG_NOSIGNAL);
    ::io_uring_sqe_set_data64(sqe, static_cast<uint64_t>(userData));
    try { registerCallback(userData, std::move(callback)); } catch (...) { return false; }
    inflight_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool IoUringPoller::submitSendmsg(socket_t fd,
                                   const struct msghdr* msg,
                                   uintptr_t userData,
                                   IoUringCompletionFn callback) noexcept {
    if (!initialized_) return false;
    io_uring_sqe* sqe = getSqe();
    if (!sqe) return false;
    ::io_uring_prep_sendmsg(sqe, static_cast<int>(fd), msg, MSG_NOSIGNAL);
    ::io_uring_sqe_set_data64(sqe, static_cast<uint64_t>(userData));
    try { registerCallback(userData, std::move(callback)); } catch (...) { return false; }
    inflight_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool IoUringPoller::submitRecvmsg(socket_t fd,
                                   struct msghdr* msg,
                                   uintptr_t userData,
                                   IoUringCompletionFn callback) noexcept {
    if (!initialized_) return false;
    io_uring_sqe* sqe = getSqe();
    if (!sqe) return false;
    ::io_uring_prep_recvmsg(sqe, static_cast<int>(fd), msg, 0);
    ::io_uring_sqe_set_data64(sqe, static_cast<uint64_t>(userData));
    try { registerCallback(userData, std::move(callback)); } catch (...) { return false; }
    inflight_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool IoUringPoller::cancelOp(uintptr_t userData) noexcept {
    if (!initialized_) return false;
    io_uring_sqe* sqe = getSqe();
    if (!sqe) return false;
    ::io_uring_prep_cancel64(sqe, static_cast<uint64_t>(userData), 0);
    ::io_uring_sqe_set_data64(sqe, 0);
    return true;
}

// ── Completion processing ────────────────────────────────────────────────────

int IoUringPoller::flush() noexcept {
    if (!initialized_) return 0;
    return ::io_uring_submit(&ring_);
}

int IoUringPoller::processCompletions(int timeoutMs) noexcept {
    if (!initialized_) return -1;

    // Submit any pending SQEs.
    ::io_uring_submit(&ring_);

    int processed = 0;
    io_uring_cqe* cqe = nullptr;

    if (timeoutMs < 0) {
        // Block indefinitely until at least one CQE.
        const int ret = ::io_uring_wait_cqe(&ring_, &cqe);
        if (ret < 0) return ret;
    } else if (timeoutMs == 0) {
        // Non-blocking peek.
        const int ret = ::io_uring_peek_cqe(&ring_, &cqe);
        if (ret < 0) return 0;
    } else {
        __kernel_timespec ts{};
        ts.tv_sec  = timeoutMs / 1000;
        ts.tv_nsec = static_cast<long long>(timeoutMs % 1000) * 1'000'000LL;
        const int ret = ::io_uring_wait_cqe_timeout(&ring_, &cqe, &ts);
        if (ret == -ETIME) return 0;
        if (ret < 0)       return ret;
    }

    // Drain all ready CQEs.
    unsigned head = 0;
    io_uring_for_each_cqe(&ring_, head, cqe) {
        const uintptr_t     ud  = static_cast<uintptr_t>(cqe->user_data);
        const int32_t       res = cqe->res;
        IoUringCompletionFn cb  = takeCallback(ud);
        if (cb) {
            try { cb(res, ud); } catch (...) {}
        }
        inflight_.fetch_sub(1, std::memory_order_relaxed);
        ++processed;
    }
    ::io_uring_cq_advance(&ring_, static_cast<unsigned>(processed));

    return processed;
}

} // namespace FastNet

#endif  // FASTNET_ENABLE_IOURING
#endif  // __linux__
