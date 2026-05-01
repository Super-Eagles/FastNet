/**
 * @file PerformanceMonitor.h
 * @brief FastNet runtime metrics API
 */
#pragma once

#include "Config.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace FastNet {

enum class MetricType {
    Counter,
    Gauge,
    Histogram,
    Timer
};

struct FASTNET_API MetricSnapshot {
    std::string name;
    MetricType type = MetricType::Counter;
    uint64_t value = 0;
    uint64_t min = 0;
    uint64_t max = 0;
    uint64_t sum = 0;
    uint64_t count = 0;
    uint64_t average = 0;
};

struct FASTNET_API PerformanceSnapshot {
    std::chrono::system_clock::time_point timestamp{};
    bool enabled = false;
    std::vector<MetricSnapshot> metrics;
};

class FASTNET_API PerformanceMonitor {
public:
    PerformanceMonitor();
    ~PerformanceMonitor();

    PerformanceMonitor(const PerformanceMonitor&) = delete;
    PerformanceMonitor& operator=(const PerformanceMonitor&) = delete;

    void initialize(bool enabled = true);
    void shutdown();

    void incrementMetric(const std::string& name, uint64_t value = 1);
    void setMetric(const std::string& name, uint64_t value);
    void updateHistogram(const std::string& name, uint64_t value);
    void recordTimer(const std::string& name, uint64_t milliseconds);

    uint64_t startTimer();
    void endTimer(const std::string& name, uint64_t timerId);

    uint64_t getMetricValue(const std::string& name) const;
    bool getMetricStats(const std::string& name, uint64_t& min, uint64_t& max, uint64_t& avg) const;
    bool getMetricSnapshot(const std::string& name, MetricSnapshot& snapshot) const;
    std::vector<MetricSnapshot> snapshotMetrics() const;
    PerformanceSnapshot snapshot() const;

    void resetMetric(const std::string& name);
    void resetAllMetrics();
    std::string exportMetricsToJson() const;

    void setEnabled(bool enabled);
    bool isEnabled() const;
    bool isInitialized() const;

private:
    struct MetricState;

    std::shared_ptr<MetricState> createMetricIfNotExists(const std::string& name, MetricType type);
    static MetricSnapshot buildSnapshot(const MetricState& metric);

    std::unordered_map<std::string, std::shared_ptr<MetricState>> metrics_;
    mutable std::mutex metricsMutex_;
    std::unordered_map<uint64_t, std::chrono::steady_clock::time_point> timerStarts_;
    mutable std::mutex timerMutex_;
    std::atomic<uint64_t> nextTimerId_{1};
    std::atomic<bool> enabled_{true};
    std::atomic<bool> initialized_{false};
};

FASTNET_API PerformanceMonitor& getPerformanceMonitor();
extern FASTNET_API PerformanceMonitor g_performanceMonitor;

#define FASTNET_PERF_INCREMENT(name, value)                         \
    do {                                                            \
        auto& __fastnet_perf = ::FastNet::getPerformanceMonitor();  \
        if (__fastnet_perf.isEnabled()) {                           \
            __fastnet_perf.incrementMetric((name), (value));        \
        }                                                           \
    } while (false)

#define FASTNET_PERF_SET(name, value)                               \
    do {                                                            \
        auto& __fastnet_perf = ::FastNet::getPerformanceMonitor();  \
        if (__fastnet_perf.isEnabled()) {                           \
            __fastnet_perf.setMetric((name), (value));              \
        }                                                           \
    } while (false)

#define FASTNET_PERF_HISTOGRAM(name, value)                         \
    do {                                                            \
        auto& __fastnet_perf = ::FastNet::getPerformanceMonitor();  \
        if (__fastnet_perf.isEnabled()) {                           \
            __fastnet_perf.updateHistogram((name), (value));        \
        }                                                           \
    } while (false)

#define FASTNET_PERF_TIMER_START(name)                                              \
    uint64_t __timer_id_##name = 0;                                                 \
    do {                                                                            \
        auto& __fastnet_perf = ::FastNet::getPerformanceMonitor();                  \
        if (__fastnet_perf.isEnabled()) {                                           \
            __timer_id_##name = __fastnet_perf.startTimer();                        \
        }                                                                           \
    } while (false)

#define FASTNET_PERF_TIMER_END(name)                                \
    do {                                                            \
        auto& __fastnet_perf = ::FastNet::getPerformanceMonitor();  \
        if (__fastnet_perf.isEnabled()) {                           \
            __fastnet_perf.endTimer(#name, __timer_id_##name);      \
        }                                                           \
    } while (false)

// Generic start/end for when the timer id needs to be stored externally.
#define FASTNET_PERF_START_TIMER()                                   \
    (::FastNet::getPerformanceMonitor().isEnabled() ?                \
         ::FastNet::getPerformanceMonitor().startTimer()             \
                                                   : 0)

#define FASTNET_PERF_END_TIMER(metric, timerId)                     \
    do {                                                            \
        auto& __fastnet_perf = ::FastNet::getPerformanceMonitor();  \
        if (__fastnet_perf.isEnabled()) {                           \
            __fastnet_perf.endTimer((metric), (timerId));           \
        }                                                           \
    } while (false)

} // namespace FastNet
