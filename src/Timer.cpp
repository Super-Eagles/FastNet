/**
 * @file Timer.cpp
 * @brief FastNet timer implementation
 */
#include "Timer.h"

#include <algorithm>
#include <memory>
#include <utility>

namespace FastNet {

namespace {

std::mutex g_globalTimerManagerMutex;
std::shared_ptr<TimerManager> g_globalTimerManager;

} // namespace

std::shared_ptr<TimerManager> getGlobalTimerManager() {
    std::lock_guard<std::mutex> lock(g_globalTimerManagerMutex);
    if (!g_globalTimerManager) {
        g_globalTimerManager = std::make_shared<TimerManager>(Duration(1));
    }
    g_globalTimerManager->start();
    return g_globalTimerManager;
}

void shutdownGlobalTimerManager() {
    std::shared_ptr<TimerManager> manager;
    {
        std::lock_guard<std::mutex> lock(g_globalTimerManagerMutex);
        manager = std::move(g_globalTimerManager);
    }
    if (manager) {
        manager->stop();
    }
}

TimerManager::TimerManager(Duration tickInterval)
    : tickInterval_((std::max)(Duration(1), tickInterval)) {}

TimerManager::~TimerManager() {
    stop();
}

void TimerManager::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }

    if (workerThread_.joinable()) {
        workerThread_.join();
    }

    workerThread_ = std::thread(&TimerManager::workerLoop, this);
}

void TimerManager::stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    condition_.notify_all();
    if (workerThread_.joinable()) {
        workerThread_.join();
    }

    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& entry : activeTimers_) {
        if (entry.second) {
            entry.second->cancelled.store(true, std::memory_order_release);
        }
    }
    while (!timers_.empty()) {
        timers_.pop();
    }
    activeTimers_.clear();
}

bool TimerManager::isRunning() const noexcept {
    return running_.load(std::memory_order_acquire);
}

TimerId TimerManager::addTimer(Duration delay, TimerCallback callback) {
    return schedule(delay, std::move(callback), false);
}

TimerId TimerManager::addRepeatingTimer(Duration interval, TimerCallback callback) {
    return schedule(interval, std::move(callback), true);
}

bool TimerManager::cancelTimer(TimerId timerId) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = activeTimers_.find(timerId);
    if (it == activeTimers_.end()) {
        return false;
    }

    it->second->cancelled.store(true, std::memory_order_release);
    activeTimers_.erase(it);
    condition_.notify_all();
    return true;
}

size_t TimerManager::getActiveTimerCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return activeTimers_.size();
}

TimerId TimerManager::schedule(Duration delay, TimerCallback callback, bool repeat) {
    if (!callback) {
        return 0;
    }

    start();

    auto entry = std::make_shared<TimerEntry>();
    entry->id = generateTimerId();
    entry->expireTime = std::chrono::steady_clock::now() + (std::max)(Duration(0), delay);
    entry->interval = (std::max)(Duration(1), delay);
    entry->callback = std::move(callback);
    entry->repeat = repeat;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        activeTimers_[entry->id] = entry;
        timers_.push(entry);
    }

    condition_.notify_all();
    return entry->id;
}

void TimerManager::workerLoop() {
    while (running_.load(std::memory_order_acquire)) {
        std::shared_ptr<TimerEntry> entry;

        {
            std::unique_lock<std::mutex> lock(mutex_);
            while (running_.load(std::memory_order_acquire)) {
                if (timers_.empty()) {
                    condition_.wait(lock, [this]() {
                        return !running_.load(std::memory_order_acquire) || !timers_.empty();
                    });
                    continue;
                }

                const auto candidate = timers_.top();
                if (!candidate || candidate->cancelled.load(std::memory_order_acquire)) {
                    timers_.pop();
                    continue;
                }

                const auto now = std::chrono::steady_clock::now();
                if (candidate->expireTime > now) {
                    condition_.wait_until(lock, candidate->expireTime);
                    continue;
                }

                entry = candidate;
                timers_.pop();
                activeTimers_.erase(entry->id);
                break;
            }
        }

        if (!running_.load(std::memory_order_acquire)) {
            break;
        }
        if (!entry || entry->cancelled.load(std::memory_order_acquire)) {
            continue;
        }

        try {
            entry->callback();
        } catch (...) {
            // Timer callbacks must not terminate the scheduler thread.
        }

        if (!entry->repeat ||
            entry->cancelled.load(std::memory_order_acquire) ||
            !running_.load(std::memory_order_acquire)) {
            continue;
        }

        const auto now = std::chrono::steady_clock::now();
        TimePoint nextExpire = entry->expireTime + entry->interval;
        if (nextExpire <= now) {
            nextExpire = now + entry->interval;
        }
        entry->expireTime = nextExpire;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!entry->cancelled.load(std::memory_order_acquire) &&
                running_.load(std::memory_order_acquire)) {
                activeTimers_[entry->id] = entry;
                timers_.push(entry);
            }
        }
        condition_.notify_all();
    }
}

TimerId TimerManager::generateTimerId() {
    return nextTimerId_.fetch_add(1, std::memory_order_relaxed);
}

Timer::Timer(IoService& ioService)
    : ioService_(ioService),
      timerManager_(getGlobalTimerManager()),
      stateToken_(std::make_shared<std::atomic<bool>>(false)) {}

Timer::~Timer() {
    stop();
}

void Timer::start(Duration delay, TimerCallback callback, bool repeat) {
    stop();
    if (!callback) {
        return;
    }

    stateToken_ = std::make_shared<std::atomic<bool>>(true);
    auto token = stateToken_;
    auto& ioService = ioService_;
    auto callbackHolder = std::make_shared<TimerCallback>(std::move(callback));

    const auto wrapped = [token, callbackHolder, repeat, &ioService]() {
        if (!token->load(std::memory_order_acquire)) {
            return;
        }

        if (repeat) {
            ioService.post([token, callbackHolder]() {
                if (!token->load(std::memory_order_acquire)) {
                    return;
                }
                (*callbackHolder)();
            });
            return;
        }

        ioService.post([token, callbackHolder]() {
            if (!token->exchange(false, std::memory_order_acq_rel)) {
                return;
            }
            (*callbackHolder)();
        });
    };

    timerId_ = repeat ? timerManager_->addRepeatingTimer(delay, wrapped)
                      : timerManager_->addTimer(delay, wrapped);
}

void Timer::stop() {
    if (stateToken_) {
        stateToken_->store(false, std::memory_order_release);
    }

    if (timerId_ != 0 && timerManager_) {
        timerManager_->cancelTimer(timerId_);
    }
    timerId_ = 0;
}

bool Timer::isRunning() const {
    return stateToken_ && stateToken_->load(std::memory_order_acquire);
}

ConnectionTimeoutManager::ConnectionTimeoutManager(std::shared_ptr<TimerManager> timerMgr)
    : timerManager_(std::move(timerMgr)) {}

void ConnectionTimeoutManager::setConnectionTimeout(ConnectionId connId,
                                                    Duration timeout,
                                                    TimeoutCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto existing = connections_.find(connId);
    if (existing != connections_.end()) {
        timerManager_->cancelTimer(existing->second.timerId);
    }

    ConnectionTimeout entry;
    entry.connId   = connId;
    entry.timeout  = timeout;
    entry.callback = callback;                    // keep a copy in the map entry
    // Lambda captures a separate copy so it remains valid even if the map entry
    // is removed before the timer fires.
    auto timerId = timerManager_->addTimer(timeout, [connId, cb = std::move(callback)]() {
        if (cb) {
            cb(connId);
        }
    });
    entry.timerId = timerId;

    connections_[connId] = std::move(entry);
}

void ConnectionTimeoutManager::refreshConnection(ConnectionId connId) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = connections_.find(connId);
    if (it == connections_.end()) {
        return;
    }

    timerManager_->cancelTimer(it->second.timerId);
    const auto timeout = it->second.timeout;
    const auto callback = it->second.callback;
    it->second.timerId = timerManager_->addTimer(timeout, [connId, callback]() {
        if (callback) {
            callback(connId);
        }
    });
}

void ConnectionTimeoutManager::removeConnection(ConnectionId connId) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = connections_.find(connId);
    if (it == connections_.end()) {
        return;
    }

    timerManager_->cancelTimer(it->second.timerId);
    connections_.erase(it);
}

size_t ConnectionTimeoutManager::getManagedConnectionCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return connections_.size();
}

} // namespace FastNet
