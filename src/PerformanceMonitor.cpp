/**
 * @file PerformanceMonitor.cpp
 * @brief FastNet runtime metrics implementation
 */
#include "PerformanceMonitor.h"

#include <algorithm>
#include <array>
#include <limits>
#include <sstream>
#include <utility>

namespace FastNet {

namespace {

constexpr std::array<const char*, 9> kDefaultMetricNames{{
    "connections.total",
    "connections.active",
    "messages.received",
    "messages.sent",
    "bytes.received",
    "bytes.sent",
    "errors.total",
    "handshake.time",
    "message.processing.time",
}};

constexpr std::array<MetricType, 9> kDefaultMetricTypes{{
    MetricType::Counter,
    MetricType::Gauge,
    MetricType::Counter,
    MetricType::Counter,
    MetricType::Counter,
    MetricType::Counter,
    MetricType::Counter,
    MetricType::Timer,
    MetricType::Timer,
}};

std::string metricTypeToString(MetricType type) {
    switch (type) {
        case MetricType::Counter:
            return "counter";
        case MetricType::Gauge:
            return "gauge";
        case MetricType::Histogram:
            return "histogram";
        case MetricType::Timer:
            return "timer";
        default:
            return "unknown";
    }
}

std::string escapeJson(std::string_view text) {
    std::string escaped;
    escaped.reserve(text.size() + 8);
    for (char ch : text) {
        switch (ch) {
            case '\\':
                escaped += "\\\\";
                break;
            case '"':
                escaped += "\\\"";
                break;
            case '\b':
                escaped += "\\b";
                break;
            case '\f':
                escaped += "\\f";
                break;
            case '\n':
                escaped += "\\n";
                break;
            case '\r':
                escaped += "\\r";
                break;
            case '\t':
                escaped += "\\t";
                break;
            default:
                escaped.push_back(ch);
                break;
        }
    }
    return escaped;
}

} // namespace

struct PerformanceMonitor::MetricState {
    explicit MetricState(std::string metricName, MetricType metricType)
        : name(std::move(metricName)),
          type(metricType),
          minValue((std::numeric_limits<uint64_t>::max)()) {}

    std::string name;
    MetricType type;
    std::atomic<uint64_t> value{0};
    uint64_t minValue = 0;
    uint64_t maxValue = 0;
    uint64_t sumValue = 0;
    uint64_t count = 0;
    mutable std::mutex mutex;
};

PerformanceMonitor g_performanceMonitor;

PerformanceMonitor& getPerformanceMonitor() {
    return g_performanceMonitor;
}

PerformanceMonitor::PerformanceMonitor() = default;

PerformanceMonitor::~PerformanceMonitor() {
    shutdown();
}

void PerformanceMonitor::initialize(bool enabled) {
    for (size_t i = 0; i < kDefaultMetricNames.size(); ++i) {
        createMetricIfNotExists(kDefaultMetricNames[i], kDefaultMetricTypes[i]);
    }
    createMetricIfNotExists("frame.size", MetricType::Histogram);
    initialized_.store(true, std::memory_order_release);
    setEnabled(enabled);
}

void PerformanceMonitor::shutdown() {
    initialized_.store(false, std::memory_order_release);
    enabled_.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(metricsMutex_);
        metrics_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(timerMutex_);
        timerStarts_.clear();
    }
    nextTimerId_.store(1, std::memory_order_release);
}

void PerformanceMonitor::incrementMetric(const std::string& name, uint64_t value) {
    if (!isEnabled()) {
        return;
    }

    if (const auto metric = createMetricIfNotExists(name, MetricType::Counter)) {
        metric->value.fetch_add(value, std::memory_order_relaxed);
    }
}

void PerformanceMonitor::setMetric(const std::string& name, uint64_t value) {
    if (!isEnabled()) {
        return;
    }

    if (const auto metric = createMetricIfNotExists(name, MetricType::Gauge)) {
        metric->value.store(value, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(metric->mutex);
        metric->minValue = std::min(metric->minValue, value);
        metric->maxValue = std::max(metric->maxValue, value);
        metric->sumValue += value;
        ++metric->count;
    }
}

void PerformanceMonitor::updateHistogram(const std::string& name, uint64_t value) {
    if (!isEnabled()) {
        return;
    }

    if (const auto metric = createMetricIfNotExists(name, MetricType::Histogram)) {
        std::lock_guard<std::mutex> lock(metric->mutex);
        metric->value.fetch_add(1, std::memory_order_relaxed);
        metric->minValue = std::min(metric->minValue, value);
        metric->maxValue = std::max(metric->maxValue, value);
        metric->sumValue += value;
        ++metric->count;
    }
}

void PerformanceMonitor::recordTimer(const std::string& name, uint64_t milliseconds) {
    if (!isEnabled()) {
        return;
    }

    if (const auto metric = createMetricIfNotExists(name, MetricType::Timer)) {
        std::lock_guard<std::mutex> lock(metric->mutex);
        metric->value.store(milliseconds, std::memory_order_relaxed);
        metric->minValue = std::min(metric->minValue, milliseconds);
        metric->maxValue = std::max(metric->maxValue, milliseconds);
        metric->sumValue += milliseconds;
        ++metric->count;
    }
}

uint64_t PerformanceMonitor::startTimer() {
    if (!isEnabled()) {
        return 0;
    }

    const uint64_t timerId = nextTimerId_.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(timerMutex_);
    timerStarts_[timerId] = std::chrono::steady_clock::now();
    return timerId;
}

void PerformanceMonitor::endTimer(const std::string& name, uint64_t timerId) {
    if (!isEnabled() || timerId == 0) {
        return;
    }

    std::chrono::steady_clock::time_point startedAt;
    {
        std::lock_guard<std::mutex> lock(timerMutex_);
        const auto it = timerStarts_.find(timerId);
        if (it == timerStarts_.end()) {
            return;
        }
        startedAt = it->second;
        timerStarts_.erase(it);
    }

    const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - startedAt);
    recordTimer(name, static_cast<uint64_t>(duration.count()));
}

uint64_t PerformanceMonitor::getMetricValue(const std::string& name) const {
    std::lock_guard<std::mutex> lock(metricsMutex_);
    const auto it = metrics_.find(name);
    return it == metrics_.end() ? 0 : it->second->value.load(std::memory_order_relaxed);
}

bool PerformanceMonitor::getMetricStats(const std::string& name,
                                        uint64_t& min,
                                        uint64_t& max,
                                        uint64_t& avg) const {
    MetricSnapshot metric;
    if (!getMetricSnapshot(name, metric)) {
        return false;
    }

    min = metric.min;
    max = metric.max;
    avg = metric.average;
    return true;
}

bool PerformanceMonitor::getMetricSnapshot(const std::string& name, MetricSnapshot& snapshot) const {
    std::lock_guard<std::mutex> lock(metricsMutex_);
    const auto it = metrics_.find(name);
    if (it == metrics_.end()) {
        return false;
    }

    snapshot = buildSnapshot(*it->second);
    return true;
}

std::vector<MetricSnapshot> PerformanceMonitor::snapshotMetrics() const {
    std::vector<MetricSnapshot> snapshots;
    {
        std::lock_guard<std::mutex> lock(metricsMutex_);
        snapshots.reserve(metrics_.size());
        for (const auto& entry : metrics_) {
            snapshots.push_back(buildSnapshot(*entry.second));
        }
    }

    std::sort(snapshots.begin(), snapshots.end(), [](const MetricSnapshot& lhs, const MetricSnapshot& rhs) {
        return lhs.name < rhs.name;
    });
    return snapshots;
}

PerformanceSnapshot PerformanceMonitor::snapshot() const {
    PerformanceSnapshot result;
    result.timestamp = std::chrono::system_clock::now();
    result.enabled = isEnabled();
    result.metrics = snapshotMetrics();
    return result;
}

void PerformanceMonitor::resetMetric(const std::string& name) {
    std::lock_guard<std::mutex> lock(metricsMutex_);
    const auto it = metrics_.find(name);
    if (it == metrics_.end()) {
        return;
    }

    auto& metric = *it->second;
    std::lock_guard<std::mutex> metricLock(metric.mutex);
    metric.value.store(0, std::memory_order_relaxed);
    metric.minValue = (std::numeric_limits<uint64_t>::max)();
    metric.maxValue = 0;
    metric.sumValue = 0;
    metric.count = 0;
}

void PerformanceMonitor::resetAllMetrics() {
    std::lock_guard<std::mutex> lock(metricsMutex_);
    for (auto& entry : metrics_) {
        auto& metric = *entry.second;
        std::lock_guard<std::mutex> metricLock(metric.mutex);
        metric.value.store(0, std::memory_order_relaxed);
        metric.minValue = (std::numeric_limits<uint64_t>::max)();
        metric.maxValue = 0;
        metric.sumValue = 0;
        metric.count = 0;
    }
}

std::string PerformanceMonitor::exportMetricsToJson() const {
    const PerformanceSnapshot data = snapshot();
    const auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(
        data.timestamp.time_since_epoch());

    std::ostringstream output;
    output << "{\n";
    output << "  \"timestamp\": " << timestamp.count() << ",\n";
    output << "  \"enabled\": " << (data.enabled ? "true" : "false") << ",\n";
    output << "  \"metrics\": [\n";

    for (size_t index = 0; index < data.metrics.size(); ++index) {
        const MetricSnapshot& metric = data.metrics[index];
        output << "    {\n";
        output << "      \"name\": \"" << escapeJson(metric.name) << "\",\n";
        output << "      \"type\": \"" << metricTypeToString(metric.type) << "\",\n";
        output << "      \"value\": " << metric.value << ",\n";
        output << "      \"min\": " << metric.min << ",\n";
        output << "      \"max\": " << metric.max << ",\n";
        output << "      \"sum\": " << metric.sum << ",\n";
        output << "      \"count\": " << metric.count << ",\n";
        output << "      \"avg\": " << metric.average << '\n';
        output << "    }";
        if (index + 1 < data.metrics.size()) {
            output << ',';
        }
        output << '\n';
    }

    output << "  ]\n";
    output << "}\n";
    return output.str();
}

void PerformanceMonitor::setEnabled(bool enabled) {
    if (!enabled) {
        std::lock_guard<std::mutex> lock(timerMutex_);
        timerStarts_.clear();
    }
    enabled_.store(enabled, std::memory_order_release);
}

bool PerformanceMonitor::isEnabled() const {
    return enabled_.load(std::memory_order_acquire);
}

bool PerformanceMonitor::isInitialized() const {
    return initialized_.load(std::memory_order_acquire);
}

std::shared_ptr<PerformanceMonitor::MetricState> PerformanceMonitor::createMetricIfNotExists(
    const std::string& name,
    MetricType type) {
    // Fast path: read lock check (most calls will hit an existing metric).
    {
        std::lock_guard<std::mutex> rlock(metricsMutex_);
        const auto it = metrics_.find(name);
        if (it != metrics_.end()) {
            return it->second->type == type ? it->second : nullptr;
        }
    }

    // Slow path: create under exclusive lock, then re-check to avoid duplicate creation.
    std::lock_guard<std::mutex> wlock(metricsMutex_);
    const auto it2 = metrics_.find(name);
    if (it2 != metrics_.end()) {
        return it2->second->type == type ? it2->second : nullptr;
    }

    auto metric = std::make_shared<MetricState>(name, type);
    metrics_.emplace(name, metric);
    return metric;
}

MetricSnapshot PerformanceMonitor::buildSnapshot(const MetricState& metric) {
    MetricSnapshot snapshot;
    snapshot.name = metric.name;
    snapshot.type = metric.type;
    snapshot.value = metric.value.load(std::memory_order_relaxed);

    std::lock_guard<std::mutex> lock(metric.mutex);
    snapshot.sum = metric.sumValue;
    snapshot.count = metric.count;

    if (metric.count == 0) {
        if (metric.type == MetricType::Counter || metric.type == MetricType::Gauge) {
            snapshot.min = snapshot.value;
            snapshot.max = snapshot.value;
            snapshot.average = snapshot.value;
        } else {
            snapshot.min = 0;
            snapshot.max = 0;
            snapshot.average = 0;
        }
        return snapshot;
    }

    snapshot.min = metric.minValue == (std::numeric_limits<uint64_t>::max)() ? 0 : metric.minValue;
    snapshot.max = metric.maxValue;
    snapshot.average = metric.sumValue / metric.count;
    return snapshot;
}

} // namespace FastNet
