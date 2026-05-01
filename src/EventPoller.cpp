/**
 * @file EventPoller.cpp
 * @brief FastNet readiness-based event poller implementation
 */
#include "EventPoller.h"

#include <algorithm>
#include <mutex>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
#else
#include <errno.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#endif

namespace FastNet {

namespace {

struct ReadyEvent {
    socket_t fd = INVALID_SOCKET_FD;
    EventType events = EventType::None;
};

#ifdef _WIN32
short toNativeEvents(EventType events) {
    short nativeEvents = 0;
    if (hasEvent(events, EventType::Read)) {
        nativeEvents |= POLLIN | POLLRDNORM;
    }
    if (hasEvent(events, EventType::Write)) {
        nativeEvents |= POLLOUT | POLLWRNORM;
    }
    if (hasEvent(events, EventType::Error)) {
        nativeEvents |= POLLERR;
    }
    if (hasEvent(events, EventType::Close)) {
        nativeEvents |= POLLHUP;
    }
    return nativeEvents;
}

EventType fromNativeEvents(short nativeEvents) {
    EventType events = EventType::None;
    if ((nativeEvents & (POLLRDNORM | POLLRDBAND | POLLIN)) != 0) {
        events |= EventType::Read;
    }
    if ((nativeEvents & (POLLWRNORM | POLLOUT)) != 0) {
        events |= EventType::Write;
    }
    if ((nativeEvents & POLLERR) != 0) {
        events |= EventType::Error;
    }
    if ((nativeEvents & (POLLHUP | POLLNVAL)) != 0) {
        events |= EventType::Close;
    }
    return events;
}

bool setSocketNonBlocking(socket_t fd) {
    u_long mode = 1;
    return ::ioctlsocket(fd, FIONBIO, &mode) == 0;
}

void closeWakeupSocket(socket_t& fd) {
    if (fd != INVALID_SOCKET_FD) {
        ::closesocket(fd);
        fd = INVALID_SOCKET_FD;
    }
}

bool createWakeupSockets(socket_t& readSocket, socket_t& writeSocket) {
    readSocket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (readSocket == INVALID_SOCKET_FD) {
        return false;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (::bind(readSocket,
               reinterpret_cast<const sockaddr*>(&address),
               static_cast<int>(sizeof(address))) != 0) {
        closeWakeupSocket(readSocket);
        return false;
    }

    sockaddr_in boundAddress{};
    int boundLength = static_cast<int>(sizeof(boundAddress));
    if (::getsockname(readSocket,
                      reinterpret_cast<sockaddr*>(&boundAddress),
                      &boundLength) != 0) {
        closeWakeupSocket(readSocket);
        return false;
    }

    writeSocket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (writeSocket == INVALID_SOCKET_FD) {
        closeWakeupSocket(readSocket);
        return false;
    }

    if (::connect(writeSocket,
                  reinterpret_cast<const sockaddr*>(&boundAddress),
                  boundLength) != 0) {
        closeWakeupSocket(writeSocket);
        closeWakeupSocket(readSocket);
        return false;
    }

    if (!setSocketNonBlocking(readSocket) || !setSocketNonBlocking(writeSocket)) {
        closeWakeupSocket(writeSocket);
        closeWakeupSocket(readSocket);
        return false;
    }

    return true;
}

void drainWakeupSocket(socket_t fd) {
    char buffer[64];
    while (fd != INVALID_SOCKET_FD) {
        const int received = ::recv(fd, buffer, static_cast<int>(sizeof(buffer)), 0);
        if (received > 0) {
            continue;
        }
        if (received == 0) {
            break;
        }

        const int error = WSAGetLastError();
        if (error == WSAEWOULDBLOCK) {
            break;
        }
        break;
    }
}

void addToFdSet(fd_set& set, socket_t fd) noexcept {
    if (set.fd_count < FD_SETSIZE) {
        FD_SET(fd, &set);
    }
}

bool isFdSetFull(const fd_set& set) noexcept {
    return set.fd_count >= FD_SETSIZE;
}
#else
uint32_t toNativeEvents(EventType events, bool oneShot) {
    uint32_t nativeEvents = 0;
    if (hasEvent(events, EventType::Read))  { nativeEvents |= EPOLLIN;  }
    if (hasEvent(events, EventType::Write)) { nativeEvents |= EPOLLOUT; }
    nativeEvents |= EPOLLRDHUP | EPOLLERR | EPOLLHUP;
    // Edge-triggered with one-shot: each fd fires at most once per arm.
    // This allows multiple threads to safely call epoll_wait concurrently
    // without duplicated dispatch for the same fd.
    if (oneShot) {
        nativeEvents |= EPOLLET | EPOLLONESHOT;
    }
    return nativeEvents;
}

EventType fromNativeEvents(uint32_t nativeEvents) {
    EventType events = EventType::None;
    if ((nativeEvents & (EPOLLIN | EPOLLPRI)) != 0) {
        events |= EventType::Read;
    }
    if ((nativeEvents & EPOLLOUT) != 0) {
        events |= EventType::Write;
    }
    if ((nativeEvents & EPOLLERR) != 0) {
        events |= EventType::Error;
    }
    if ((nativeEvents & (EPOLLRDHUP | EPOLLHUP)) != 0) {
        events |= EventType::Close;
    }
    return events;
}
#endif

} // namespace

class EventPoller::Impl {
public:
    bool initialize(size_t maxEvents, bool oneShot = false) {
        oneShot_ = oneShot;
        shutdown();

#ifdef _WIN32
        std::lock_guard<std::mutex> lock(mutex_);
        maxEvents_ = std::max<size_t>(1, maxEvents);
        if (!createWakeupSockets(wakeupReadSocket_, wakeupWriteSocket_)) {
            initialized_ = false;
            return false;
        }
        wakeupPending_.store(false, std::memory_order_release);
        initialized_ = true;
        return true;
#else
        epollFd_ = epoll_create1(EPOLL_CLOEXEC);
        if (epollFd_ < 0) {
            return false;
        }

        wakeupFd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (wakeupFd_ < 0) {
            ::close(epollFd_);
            epollFd_ = -1;
            return false;
        }

        epoll_event wakeEvent{};
        wakeEvent.events = EPOLLIN;
        wakeEvent.data.fd = wakeupFd_;
        if (epoll_ctl(epollFd_, EPOLL_CTL_ADD, wakeupFd_, &wakeEvent) != 0) {
            ::close(wakeupFd_);
            ::close(epollFd_);
            wakeupFd_ = -1;
            epollFd_ = -1;
            return false;
        }

        events_.assign(std::max<size_t>(1, maxEvents), epoll_event{});
        wakeupPending_.store(false, std::memory_order_release);
        return true;
#endif
    }

    void shutdown() noexcept {
#ifdef _WIN32
        std::lock_guard<std::mutex> lock(mutex_);
        pollEntries_.clear();
        indexByFd_.clear();
        initialized_ = false;
        wakeupPending_.store(false, std::memory_order_release);
        closeWakeupSocket(wakeupWriteSocket_);
        closeWakeupSocket(wakeupReadSocket_);
#else
        if (wakeupFd_ >= 0) {
            ::close(wakeupFd_);
            wakeupFd_ = -1;
        }
        if (epollFd_ >= 0) {
            ::close(epollFd_);
            epollFd_ = -1;
        }
        events_.clear();
        wakeupPending_.store(false, std::memory_order_release);
#endif
    }

    bool add(socket_t fd, EventType events) noexcept {
#ifdef _WIN32
        std::lock_guard<std::mutex> lock(mutex_);
        if (!initialized_ || indexByFd_.count(fd) != 0) {
            return false;
        }

        WSAPOLLFD entry{};
        entry.fd = fd;
        entry.events = toNativeEvents(events);
        pollEntries_.push_back(entry);
        indexByFd_[fd] = pollEntries_.size() - 1;
        snapshotDirty_ = true;  // snapshot must be rebuilt before next poll
        return true;
#else
        if (epollFd_ < 0) {
            return false;
        }

        epoll_event event{};
        event.events = toNativeEvents(events, oneShot_);
        event.data.fd = fd;
        return epoll_ctl(epollFd_, EPOLL_CTL_ADD, fd, &event) == 0;
#endif
    }

    bool modify(socket_t fd, EventType events) noexcept {
#ifdef _WIN32
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = indexByFd_.find(fd);
        if (!initialized_ || it == indexByFd_.end()) {
            return false;
        }

        pollEntries_[it->second].events = toNativeEvents(events);
        pollEntries_[it->second].revents = 0;
        snapshotDirty_ = true;
        return true;
#else
        if (epollFd_ < 0) {
            return false;
        }

        epoll_event event{};
        event.events = toNativeEvents(events, oneShot_);
        event.data.fd = fd;
        return epoll_ctl(epollFd_, EPOLL_CTL_MOD, fd, &event) == 0;
#endif
    }

    // Re-arm a one-shot fd so the next event will be delivered.
    // On Linux: same as modify() (EPOLL_CTL_MOD with EPOLLONESHOT).
    // On Windows: no-op (not applicable to WSAPoll/select model).
    bool rearm(socket_t fd, EventType events) noexcept {
#ifdef _WIN32
        (void)fd; (void)events;
        return true;  // Not needed for WSAPoll path
#else
        return modify(fd, events);
#endif
    }

    bool remove(socket_t fd) noexcept {
#ifdef _WIN32
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = indexByFd_.find(fd);
        if (!initialized_ || it == indexByFd_.end()) {
            return false;
        }

        const size_t index = it->second;
        const size_t lastIndex = pollEntries_.size() - 1;
        if (index != lastIndex) {
            pollEntries_[index] = pollEntries_[lastIndex];
            indexByFd_[pollEntries_[index].fd] = index;
        }
        pollEntries_.pop_back();
        indexByFd_.erase(it);
        snapshotDirty_ = true;
        return true;
#else
        if (epollFd_ < 0) {
            return false;
        }

        return epoll_ctl(epollFd_, EPOLL_CTL_DEL, fd, nullptr) == 0;
#endif
    }

    int poll(int timeoutMs, std::vector<ReadyEvent>& readyEvents) noexcept {
        readyEvents.clear();

#ifdef _WIN32
        socket_t wakeupSocket = INVALID_SOCKET_FD;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!initialized_) {
                return -1;
            }
            // Lazy snapshot rebuild: only when fd set changed since last poll.
            if (snapshotDirty_) {
                snapshot_.clear();
                snapshot_.reserve(pollEntries_.size() + 1);
                snapshot_.assign(pollEntries_.begin(), pollEntries_.end());
                if (wakeupReadSocket_ != INVALID_SOCKET_FD) {
                    WSAPOLLFD wakeupPollFd{};
                    wakeupPollFd.fd     = wakeupReadSocket_;
                    wakeupPollFd.events = POLLIN | POLLRDNORM;
                    snapshot_.push_back(wakeupPollFd);
                }
                snapshotDirty_ = false;
            }
            wakeupSocket = wakeupReadSocket_;
        }

        // Work from the cached snapshot_ (not locked during WSAPoll).
        // Producers that add/remove fds while we poll set snapshotDirty_=true;
        // the next poll iteration will pick up the new set.
        if (readyEvents.capacity() < snapshot_.size()) {
            readyEvents.reserve(snapshot_.size());
        }

        if (snapshot_.size() < FD_SETSIZE) {
            fd_set readSet;
            fd_set writeSet;
            fd_set errorSet;
            FD_ZERO(&readSet);
            FD_ZERO(&writeSet);
            FD_ZERO(&errorSet);

            bool hasRead = false;
            bool hasWrite = false;
            bool hasError = false;
            for (const auto& entry : snapshot_) {
                if (entry.fd == INVALID_SOCKET_FD) {
                    continue;
                }
                if ((entry.events & (POLLIN | POLLRDNORM | POLLRDBAND)) != 0 && !isFdSetFull(readSet)) {
                    addToFdSet(readSet, entry.fd);
                    hasRead = true;
                }
                if ((entry.events & (POLLOUT | POLLWRNORM)) != 0 && !isFdSetFull(writeSet)) {
                    addToFdSet(writeSet, entry.fd);
                    hasWrite = true;
                }
                if ((entry.events & (POLLERR | POLLHUP | POLLNVAL)) != 0 && !isFdSetFull(errorSet)) {
                    addToFdSet(errorSet, entry.fd);
                    hasError = true;
                }
            }

            timeval timeout{};
            timeval* timeoutPtr = nullptr;
            if (timeoutMs >= 0) {
                timeout.tv_sec = timeoutMs / 1000;
                timeout.tv_usec = (timeoutMs % 1000) * 1000;
                timeoutPtr = &timeout;
            }

            const int result = ::select(
                0,
                hasRead ? &readSet : nullptr,
                hasWrite ? &writeSet : nullptr,
                hasError ? &errorSet : nullptr,
                timeoutPtr);
            if (result < 0 && WSAGetLastError() == WSAEINTR) {
                return 0;
            }
            if (result <= 0) {
                return result;
            }

            for (const auto& entry : snapshot_) {
                if (entry.fd == wakeupSocket) {
                    if (FD_ISSET(wakeupSocket, &readSet) || FD_ISSET(wakeupSocket, &errorSet)) {
                        drainWakeupSocket(wakeupSocket);
                        wakeupPending_.store(false, std::memory_order_release);
                    }
                    continue;
                }

                short revents = 0;
                if (hasRead  && FD_ISSET(entry.fd, &readSet))  { revents |= POLLIN | POLLRDNORM; }
                if (hasWrite && FD_ISSET(entry.fd, &writeSet)) { revents |= POLLOUT | POLLWRNORM; }
                if (hasError && FD_ISSET(entry.fd, &errorSet)) { revents |= POLLERR; }
                if (revents != 0) {
                    readyEvents.push_back(ReadyEvent{entry.fd, fromNativeEvents(revents)});
                }
            }
            return static_cast<int>(readyEvents.size());
        }

        const int result = ::WSAPoll(snapshot_.data(), static_cast<ULONG>(snapshot_.size()), timeoutMs);
        if (result < 0 && WSAGetLastError() == WSAEINTR) {
            return 0;
        }
        if (result <= 0) {
            return result;
        }

        for (const auto& entry : snapshot_) {
            if (entry.fd == wakeupSocket) {
                if (entry.revents & (POLLIN | POLLRDNORM | POLLERR | POLLHUP)) {
                    drainWakeupSocket(wakeupSocket);
                    wakeupPending_.store(false, std::memory_order_release);
                }
                continue;
            }

            if (entry.revents != 0) {
                readyEvents.push_back(ReadyEvent{entry.fd, fromNativeEvents(entry.revents)});
            }
        }
        return static_cast<int>(readyEvents.size());
#else
        if (epollFd_ < 0) {
            return -1;
        }

        const int result = epoll_wait(epollFd_,
                                      events_.data(),
                                      static_cast<int>(events_.size()),
                                      timeoutMs);
        if (result < 0 && errno == EINTR) {
            return 0;
        }
        if (result <= 0) {
            return result;
        }

        for (int i = 0; i < result; ++i) {
            const epoll_event& event = events_[static_cast<size_t>(i)];
            const socket_t fd = static_cast<socket_t>(event.data.fd);
            if (fd == wakeupFd_) {
                std::uint64_t value = 0;
                while (::read(wakeupFd_, &value, sizeof(value)) == sizeof(value)) {
                }
                wakeupPending_.store(false, std::memory_order_release);
                continue;
            }

            readyEvents.push_back(ReadyEvent{fd, fromNativeEvents(event.events)});
        }
        return static_cast<int>(readyEvents.size());
#endif
    }

    bool wakeup() noexcept {
#ifdef _WIN32
        socket_t wakeupSocket = INVALID_SOCKET_FD;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!initialized_ || wakeupWriteSocket_ == INVALID_SOCKET_FD) {
                return false;
            }
            wakeupSocket = wakeupWriteSocket_;
        }

        bool expected = false;
        if (!wakeupPending_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            return true;
        }

        const char byte = 1;
        const int sent = ::send(wakeupSocket, &byte, 1, 0);
        if (sent == 1) {
            return true;
        }

        const int error = WSAGetLastError();
        if (error == WSAEWOULDBLOCK) {
            return true;
        }

        wakeupPending_.store(false, std::memory_order_release);
        return false;
#else
        if (wakeupFd_ < 0) {
            return false;
        }
        bool expected = false;
        if (!wakeupPending_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            return true;
        }

        const std::uint64_t value = 1;
        const ssize_t written = ::write(wakeupFd_, &value, sizeof(value));
        if (written == static_cast<ssize_t>(sizeof(value)) || errno == EAGAIN) {
            return true;
        }
        wakeupPending_.store(false, std::memory_order_release);
        return false;
#endif
    }

    // ── Private members ───────────────────────────────────────────────────
#ifdef _WIN32
    std::mutex mutex_;
    std::vector<WSAPOLLFD> pollEntries_;
    // FlatHashMap: ~3x faster lookup than unordered_map for integer keys.
    FlatHashMap<socket_t, size_t> indexByFd_;
    size_t maxEvents_ = 0;
    bool initialized_ = false;
    bool oneShot_     = false;
    // Dirty flag: set when pollEntries_ changes; snapshot is only rebuilt
    // when this is true, eliminating the O(N) copy on every poll() call.
    bool snapshotDirty_ = true;
    socket_t wakeupReadSocket_  = INVALID_SOCKET_FD;
    socket_t wakeupWriteSocket_ = INVALID_SOCKET_FD;
    std::atomic<bool> wakeupPending_{false};
    // Cached snapshot for WSAPoll — rebuilt lazily only when snapshotDirty_.
    std::vector<WSAPOLLFD> snapshot_;
#else
    int epollFd_  = -1;
    int wakeupFd_ = -1;
    bool oneShot_ = false;
    std::vector<epoll_event> events_;
    std::atomic<bool> wakeupPending_{false};
#endif
};

EventPoller::EventPoller() noexcept
    : impl_(std::make_unique<Impl>()) {}

EventPoller::~EventPoller() noexcept {
    shutdown();
}

bool EventPoller::initialize(size_t maxEvents, bool useOneShot) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    eventMap_.clear();
    eventMap_.reserve((std::max)(maxEvents, static_cast<size_t>(16)));
    oneShot_ = useOneShot;
    const bool initialized = impl_->initialize(maxEvents, useOneShot);
    running_.store(initialized, std::memory_order_release);
    return initialized;
}

void EventPoller::shutdown() noexcept {
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        eventMap_.clear();
    }

    impl_->wakeup();
    impl_->shutdown();
}

bool EventPoller::add(socket_t fd,
                      EventType events,
                      const EventCallback& callback,
                      void* userData) noexcept {
    if (!callback || !running_.load(std::memory_order_acquire)) {
        return false;
    }

    // Pre-build the shared callback wrapper before acquiring any lock:
    // make_shared may allocate, and we don't want to hold the map lock then.
    std::shared_ptr<EventCallback> sharedCb;
    try {
        sharedCb = std::make_shared<EventCallback>(callback);
    } catch (...) {
        return false;
    }

    // Phase 1: quick duplicate check under shared lock (cheap path).
    {
        std::shared_lock<std::shared_mutex> rlock(mutex_);
        if (eventMap_.count(fd) != 0) {
            return false;
        }
    }

    // Phase 2: register with the OS kernel FIRST (no eventMap_ lock held).
    // If this fails the fd never enters eventMap_ — no cleanup needed.
    // This eliminates the "insert into map → kernel fails → erase under lock"
    // race that could briefly expose an incomplete EventData to poll().
    if (!impl_->add(fd, events)) {
        return false;
    }

    // Phase 3: insert into eventMap_ under write lock.
    {
        std::unique_lock<std::shared_mutex> wlock(mutex_);
        if (eventMap_.count(fd) != 0) {
            // Another thread raced us between phase 1 and phase 3.
            impl_->remove(fd);  // roll back the kernel registration
            return false;
        }
        try {
            eventMap_[fd] = EventData{fd, events, std::move(sharedCb), userData};
        } catch (...) {
            impl_->remove(fd);
            return false;
        }
    }

    impl_->wakeup();
    return true;
}

bool EventPoller::modify(socket_t fd, EventType events) noexcept {
    if (!running_.load(std::memory_order_acquire)) {
        return false;
    }

    std::unique_lock<std::shared_mutex> wlock(mutex_);
    auto it = eventMap_.find(fd);
    if (it == eventMap_.end()) {
        return false;
    }

    if (!impl_->modify(fd, events)) {
        return false;
    }
    it->second.events = events;

    impl_->wakeup();
    return true;
}

bool EventPoller::remove(socket_t fd) noexcept {
    if (!running_.load(std::memory_order_acquire)) {
        return false;
    }

    std::unique_lock<std::shared_mutex> lock(mutex_);
    if (eventMap_.count(fd) == 0) {
        return false;
    }

    const bool removed = impl_->remove(fd);
    eventMap_.erase(fd);
    if (removed) {
        impl_->wakeup();
    }
    return removed;
}

bool EventPoller::rearm(socket_t fd, EventType events) noexcept {
    if (!running_.load(std::memory_order_acquire) || !oneShot_) {
        return true;  // no-op when one-shot mode is not active
    }
    return impl_->rearm(fd, events);
}

int EventPoller::poll(int timeoutMs) noexcept {
    if (!running_.load(std::memory_order_acquire)) {
        return 0;
    }

    thread_local std::vector<ReadyEvent> readyEvents;
    readyEvents.clear();

    const int pollResult = impl_->poll(timeoutMs, readyEvents);
    if (pollResult <= 0) {
        return pollResult;
    }

    int invoked = 0;
    for (const auto& ready : readyEvents) {
        // Two-phase dispatch:
        //  Phase 1  — hold shared_lock only to copy the shared_ptr (atomic refcount++)
        //  Phase 2  — invoke the callback WITHOUT any lock held, so callbacks can
        //             safely call add/modify/remove/rearm without deadlock.
        //
        // The shared_ptr ensures the EventCallback stays alive even if the fd is
        // concurrently removed between phase 1 and phase 2.
        std::shared_ptr<EventCallback> callbackPtr;
        void*     userData  = nullptr;
        EventType armEvents = ready.events;
        {
            std::shared_lock<std::shared_mutex> lock(mutex_);
            const auto it = eventMap_.find(ready.fd);
            if (it == eventMap_.end()) {
                // fd was removed while we were in poll(); skip silently.
                continue;
            }
            callbackPtr = it->second.callback;   // atomic refcount++ only
            userData    = it->second.userData;
            armEvents   = it->second.events;     // remember registered interest
        }

        if (callbackPtr && *callbackPtr) {
            (*callbackPtr)(ready.fd, ready.events, userData, 0);
            ++invoked;
        }

        // In EPOLLONESHOT mode: re-arm only if the callback did not remove the
        // fd. The fd may already be closed/reused after a user callback.
        if (oneShot_) {
            bool stillRegistered = false;
            {
                std::shared_lock<std::shared_mutex> lock(mutex_);
                const auto it = eventMap_.find(ready.fd);
                if (it != eventMap_.end()) {
                    armEvents = it->second.events;
                    stillRegistered = true;
                }
            }
            if (stillRegistered) {
                impl_->rearm(ready.fd, armEvents);
            }
        }
    }

    return invoked;
}

bool EventPoller::wakeup() noexcept {
    return impl_->wakeup();
}

size_t EventPoller::getSocketCount() const noexcept {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return eventMap_.size();
}

} // namespace FastNet
