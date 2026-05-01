/**
 * @file Timer.h
 * @brief FastNet timer primitives
 */
#pragma once

#include "Config.h"
#include "IoService.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>

namespace FastNet {

using TimerId = uint64_t;
using TimerCallback = std::function<void()>;
using TimePoint = std::chrono::steady_clock::time_point;
using Duration = std::chrono::milliseconds;

class FASTNET_API TimerManager {
public:
    explicit TimerManager(Duration tickInterval = Duration(10));
    ~TimerManager();

    TimerManager(const TimerManager&) = delete;
    TimerManager& operator=(const TimerManager&) = delete;

    void start();
    void stop();
    bool isRunning() const noexcept;

    TimerId addTimer(Duration delay, TimerCallback callback);
    TimerId addRepeatingTimer(Duration interval, TimerCallback callback);
    bool cancelTimer(TimerId timerId);
    size_t getActiveTimerCount() const;

private:
    struct TimerEntry {
        TimerId id = 0;
        TimePoint expireTime{};
        Duration interval{0};
        TimerCallback callback;
        bool repeat = false;
        std::atomic<bool> cancelled{false};
    };

    struct TimerCompare {
        bool operator()(const std::shared_ptr<TimerEntry>& lhs,
                        const std::shared_ptr<TimerEntry>& rhs) const {
            return lhs->expireTime > rhs->expireTime;
        }
    };

    TimerId schedule(Duration delay, TimerCallback callback, bool repeat);
    void workerLoop();
    TimerId generateTimerId();

    std::priority_queue<std::shared_ptr<TimerEntry>,
                        std::vector<std::shared_ptr<TimerEntry>>,
                        TimerCompare>
        timers_;
    std::unordered_map<TimerId, std::shared_ptr<TimerEntry>> activeTimers_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::thread workerThread_;
    std::atomic<bool> running_{false};
    Duration tickInterval_;
    std::atomic<TimerId> nextTimerId_{1};
};

FASTNET_API std::shared_ptr<TimerManager> getGlobalTimerManager();
FASTNET_API void shutdownGlobalTimerManager();

class FASTNET_API Timer {
public:
    explicit Timer(IoService& ioService);
    ~Timer();

    Timer(const Timer&) = delete;
    Timer& operator=(const Timer&) = delete;

    void start(Duration delay, TimerCallback callback, bool repeat = false);
    void stop();
    bool isRunning() const;

private:
    IoService& ioService_;
    std::shared_ptr<TimerManager> timerManager_;
    TimerId timerId_ = 0;
    std::shared_ptr<std::atomic<bool>> stateToken_;
};

class FASTNET_API ConnectionTimeoutManager {
public:
    using ConnectionId = uint64_t;
    using TimeoutCallback = std::function<void(ConnectionId)>;

    explicit ConnectionTimeoutManager(std::shared_ptr<TimerManager> timerMgr);

    void setConnectionTimeout(ConnectionId connId, Duration timeout, TimeoutCallback callback);
    void refreshConnection(ConnectionId connId);
    void removeConnection(ConnectionId connId);
    size_t getManagedConnectionCount() const;

private:
    struct ConnectionTimeout {
        ConnectionId connId = 0;
        TimerId timerId = 0;
        Duration timeout{0};
        TimeoutCallback callback;
    };

    std::shared_ptr<TimerManager> timerManager_;
    std::unordered_map<ConnectionId, ConnectionTimeout> connections_;
    mutable std::mutex mutex_;
};

} // namespace FastNet
