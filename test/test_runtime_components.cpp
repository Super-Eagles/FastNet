#include "FastNet/FastNet.h"
#include "TestSupport.h"

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

namespace {

template<typename Predicate>
bool waitFor(std::mutex& mutex,
             std::condition_variable& condition,
             std::chrono::milliseconds timeout,
             Predicate&& predicate) {
    std::unique_lock<std::mutex> lock(mutex);
    return condition.wait_for(lock, timeout, std::forward<Predicate>(predicate));
}

} // namespace

int main() {
    using namespace std::chrono_literals;

    FASTNET_TEST_ASSERT_EQ(FastNet::initialize(2), FastNet::ErrorCode::Success);
    FASTNET_TEST_ASSERT(FastNet::isInitialized());
    FASTNET_TEST_ASSERT_EQ(FastNet::initialize(2), FastNet::ErrorCode::Success);
    FASTNET_TEST_ASSERT_EQ(FastNet::initialize(3), FastNet::ErrorCode::AlreadyRunning);

    auto& ioService = FastNet::getGlobalIoService();
    FASTNET_TEST_ASSERT(ioService.isRunning());
    FASTNET_TEST_ASSERT(ioService.getThreadCount() == 2);

    std::mutex taskMutex;
    std::condition_variable taskCondition;
    size_t executedTasks = 0;
    for (size_t index = 0; index < 16; ++index) {
        ioService.post([&]() {
            std::lock_guard<std::mutex> lock(taskMutex);
            ++executedTasks;
            taskCondition.notify_all();
        });
    }
    FASTNET_TEST_ASSERT_MSG(
        waitFor(taskMutex, taskCondition, 2s, [&]() { return executedTasks == 16; }),
        "IoService should execute posted tasks");

    FastNet::TimerManager timerManager(std::chrono::milliseconds(1));
    timerManager.start();
    std::mutex timerMutex;
    std::condition_variable timerCondition;
    size_t oneShotCount = 0;
    size_t repeatingCount = 0;
    const FastNet::TimerId oneShot = timerManager.addTimer(10ms, [&]() {
        std::lock_guard<std::mutex> lock(timerMutex);
        ++oneShotCount;
        timerCondition.notify_all();
    });
    FASTNET_TEST_ASSERT(oneShot != 0);
    const FastNet::TimerId repeating = timerManager.addRepeatingTimer(5ms, [&]() {
        std::lock_guard<std::mutex> lock(timerMutex);
        ++repeatingCount;
        timerCondition.notify_all();
    });
    FASTNET_TEST_ASSERT(repeating != 0);
    FASTNET_TEST_ASSERT_MSG(
        waitFor(timerMutex, timerCondition, 1s, [&]() { return oneShotCount == 1 && repeatingCount >= 2; }),
        "TimerManager should fire one-shot and repeating timers");
    FASTNET_TEST_ASSERT(timerManager.cancelTimer(repeating));
    timerManager.stop();
    FASTNET_TEST_ASSERT(!timerManager.isRunning());

    FastNet::Timer ioTimer(ioService);
    std::mutex ioTimerMutex;
    std::condition_variable ioTimerCondition;
    bool ioTimerFired = false;
    ioTimer.start(10ms, [&]() {
        std::lock_guard<std::mutex> lock(ioTimerMutex);
        ioTimerFired = true;
        ioTimerCondition.notify_all();
    });
    FASTNET_TEST_ASSERT_MSG(
        waitFor(ioTimerMutex, ioTimerCondition, 1s, [&]() { return ioTimerFired; }),
        "Timer wrapper should fire");
    ioTimer.stop();
    FASTNET_TEST_ASSERT(!ioTimer.isRunning());

    auto& monitor = FastNet::getPerformanceMonitor();
    monitor.initialize(true);
    monitor.resetAllMetrics();
    monitor.incrementMetric("runtime.counter", 2);
    monitor.setMetric("runtime.gauge", 7);
    monitor.updateHistogram("runtime.histogram", 10);
    monitor.updateHistogram("runtime.histogram", 30);
    const uint64_t timerId = monitor.startTimer();
    std::this_thread::sleep_for(1ms);
    monitor.endTimer("runtime.timer", timerId);
    FASTNET_TEST_ASSERT_EQ(monitor.getMetricValue("runtime.counter"), static_cast<uint64_t>(2));
    FASTNET_TEST_ASSERT_EQ(monitor.getMetricValue("runtime.gauge"), static_cast<uint64_t>(7));
    uint64_t minValue = 0;
    uint64_t maxValue = 0;
    uint64_t avgValue = 0;
    FASTNET_TEST_ASSERT(monitor.getMetricStats("runtime.histogram", minValue, maxValue, avgValue));
    FASTNET_TEST_ASSERT_EQ(minValue, static_cast<uint64_t>(10));
    FASTNET_TEST_ASSERT_EQ(maxValue, static_cast<uint64_t>(30));
    FASTNET_TEST_ASSERT_EQ(avgValue, static_cast<uint64_t>(20));
    FASTNET_TEST_ASSERT(monitor.exportMetricsToJson().find("runtime.counter") != std::string::npos);

    std::filesystem::create_directories("tmp");
    const std::string logPath = "tmp/fastnet_runtime_test.log";
    std::filesystem::remove(logPath);
    auto& logger = FastNet::AsyncLogger::getInstance();
    logger.initialize(logPath, FastNet::LogLevel::TRACE, 1024 * 1024, false);
    logger.log(FastNet::LogLevel::INFO, __FILE__, __LINE__, __func__, "runtime logger message");
    logger.flush();
    logger.shutdown();
    std::ifstream logFile(logPath);
    std::string logContent((std::istreambuf_iterator<char>(logFile)), std::istreambuf_iterator<char>());
    FASTNET_TEST_ASSERT(logContent.find("runtime logger message") != std::string::npos);
    FASTNET_TEST_ASSERT_EQ(FastNet::logLevelFromString("warn"), FastNet::LogLevel::WARN_LVL);
    FASTNET_TEST_ASSERT(std::string(FastNet::logLevelToString(FastNet::LogLevel::ERROR_LVL)) == "ERROR");

    FastNet::cleanup();
    FASTNET_TEST_ASSERT(FastNet::isInitialized());
    FastNet::cleanup();
    FASTNET_TEST_ASSERT(!FastNet::isInitialized());

    std::cout << "runtime component tests passed" << '\n';
    return 0;
}
