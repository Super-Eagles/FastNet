#pragma once

#include "FastNet/BenchmarkUtils.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace FastNetExamples {

struct LatencyHistogram {
    static constexpr size_t kBucketCount = 64;
    std::array<uint64_t, kBucketCount> buckets{};
    uint64_t samples = 0;

    static size_t bucketIndex(uint64_t latencyNs) {
        size_t index = 0;
        while (latencyNs > 1 && index + 1 < kBucketCount) {
            latencyNs >>= 1U;
            ++index;
        }
        return index;
    }

    static uint64_t bucketUpperBoundNs(size_t index) {
        if (index >= kBucketCount - 1) {
            return (std::numeric_limits<uint64_t>::max)();
        }
        return 1ULL << index;
    }

    void record(uint64_t latencyNs) {
        ++buckets[bucketIndex(latencyNs)];
        ++samples;
    }

    double percentileLatencyMs(double percentile) const {
        if (samples == 0) {
            return 0.0;
        }
        if (percentile <= 0.0) {
            percentile = 0.0;
        } else if (percentile > 100.0) {
            percentile = 100.0;
        }

        uint64_t threshold = static_cast<uint64_t>(
            std::ceil((percentile / 100.0) * static_cast<double>(samples)));
        if (threshold == 0) {
            threshold = 1;
        }

        uint64_t cumulative = 0;
        for (size_t index = 0; index < kBucketCount; ++index) {
            cumulative += buckets[index];
            if (cumulative >= threshold) {
                return static_cast<double>(bucketUpperBoundNs(index)) / 1'000'000.0;
            }
        }
        return static_cast<double>(bucketUpperBoundNs(kBucketCount - 1)) / 1'000'000.0;
    }
};

struct AtomicLatencyHistogram {
    std::array<std::atomic<uint64_t>, LatencyHistogram::kBucketCount> buckets;

    AtomicLatencyHistogram() {
        reset();
    }

    void reset() {
        for (auto& bucket : buckets) {
            bucket.store(0, std::memory_order_relaxed);
        }
    }

    void record(uint64_t latencyNs) {
        const size_t index = LatencyHistogram::bucketIndex(latencyNs);
        buckets[index].fetch_add(1, std::memory_order_relaxed);
    }

    LatencyHistogram snapshot() const {
        LatencyHistogram histogram;
        for (size_t index = 0; index < LatencyHistogram::kBucketCount; ++index) {
            histogram.buckets[index] = buckets[index].load(std::memory_order_relaxed);
            histogram.samples += histogram.buckets[index];
        }
        return histogram;
    }
};

struct BenchmarkRunResult {
    double seconds = 0.0;
    uint64_t completed = 0;
    uint64_t payloadBytes = 0;
    uint64_t duplexBytes = 0;
    uint64_t totalLatencyNs = 0;
    uint64_t maxLatencyNs = 0;
    uint64_t clientErrors = 0;
    uint64_t sendAttempts = 0;
    uint64_t serverReceivedMessages = 0;
    uint64_t serverReceivedBytes = 0;
    uint64_t clientReceivedBytes = 0;
    LatencyHistogram latencyHistogram;

    double throughputOpsPerSecond() const {
        return seconds > 0.0 ? static_cast<double>(completed) / seconds : 0.0;
    }

    double offeredOpsPerSecond() const {
        return seconds > 0.0 ? static_cast<double>(sendAttempts) / seconds : 0.0;
    }

    double appBytesPerSecond() const {
        return seconds > 0.0 ? static_cast<double>(payloadBytes) / seconds : 0.0;
    }

    double wireBytesPerSecond() const {
        return seconds > 0.0 ? static_cast<double>(duplexBytes) / seconds : 0.0;
    }

    double averageLatencyMs() const {
        return completed != 0
                   ? static_cast<double>(totalLatencyNs) / static_cast<double>(completed) / 1'000'000.0
                   : 0.0;
    }

    double maxLatencyMs() const {
        return static_cast<double>(maxLatencyNs) / 1'000'000.0;
    }

    double p50LatencyMs() const {
        return latencyHistogram.percentileLatencyMs(50.0);
    }

    double p95LatencyMs() const {
        return latencyHistogram.percentileLatencyMs(95.0);
    }

    double p99LatencyMs() const {
        return latencyHistogram.percentileLatencyMs(99.0);
    }

    double completionRatio() const {
        return sendAttempts != 0
                   ? static_cast<double>(completed) / static_cast<double>(sendAttempts)
                   : 0.0;
    }
};

inline std::string formatScaledRate(double value,
                                    double step,
                                    std::initializer_list<const char*> units) {
    if (value < 0.0) {
        value = 0.0;
    }

    auto current = units.begin();
    auto next = current;
    ++next;
    while (next != units.end() && value >= step) {
        value /= step;
        current = next;
        ++next;
    }

    std::ostringstream output;
    output << std::fixed << std::setprecision(value >= 100.0 ? 1 : 2) << value << ' ' << *current;
    return output.str();
}

inline std::string formatOpsRate(double opsPerSecond) {
    return formatScaledRate(opsPerSecond, 1000.0, {"ops/s", "Kops/s", "Mops/s", "Gops/s", "Tops/s"});
}

inline std::string formatByteRate(double bytesPerSecond) {
    return formatScaledRate(bytesPerSecond, 1024.0, {"B/s", "KB/s", "MB/s", "GB/s", "TB/s"});
}

inline void printRunResult(const BenchmarkRunResult& run, size_t roundIndex) {
    std::cout << "\nRound " << roundIndex << '\n'
              << "  round trips : " << run.completed << '\n'
              << "  offered     : " << formatOpsRate(run.offeredOpsPerSecond()) << '\n'
              << "  throughput  : " << formatOpsRate(run.throughputOpsPerSecond()) << '\n'
              << "  completion  : " << std::fixed << std::setprecision(2) << (run.completionRatio() * 100.0) << "%\n"
              << "  app bw      : " << formatByteRate(run.appBytesPerSecond()) << '\n'
              << "  wire bw     : " << formatByteRate(run.wireBytesPerSecond()) << '\n';

    if (run.completed != 0) {
        std::cout << "  avg rtt     : " << FastNet::BenchmarkUtils::formatLatency(run.averageLatencyMs()) << '\n'
                  << "  p50 rtt     : " << FastNet::BenchmarkUtils::formatLatency(run.p50LatencyMs()) << '\n'
                  << "  p95 rtt     : " << FastNet::BenchmarkUtils::formatLatency(run.p95LatencyMs()) << '\n'
                  << "  p99 rtt     : " << FastNet::BenchmarkUtils::formatLatency(run.p99LatencyMs()) << '\n'
                  << "  max rtt     : " << FastNet::BenchmarkUtils::formatLatency(run.maxLatencyMs()) << '\n';
    } else {
        std::cout << "  avg rtt     : n/a\n"
                  << "  p50 rtt     : n/a\n"
                  << "  p95 rtt     : n/a\n"
                  << "  p99 rtt     : n/a\n"
                  << "  max rtt     : n/a\n";
    }

    std::cout << "  client errs : " << run.clientErrors << '\n';
    if (run.completed == 0) {
        std::cout << "  diag sends  : " << run.sendAttempts << '\n'
                  << "  diag srv rx : " << run.serverReceivedMessages << " msgs / "
                  << run.serverReceivedBytes << " bytes\n"
                  << "  diag cli rx : " << run.clientReceivedBytes << " bytes\n";
    }
}

struct BenchmarkSummary {
    size_t rounds = 0;
    size_t successfulRounds = 0;
    double meanOfferedOps = 0.0;
    double medianOfferedOps = 0.0;
    double bestOfferedOps = 0.0;
    double worstOfferedOps = 0.0;
    double meanThroughputOps = 0.0;
    double medianThroughputOps = 0.0;
    double bestThroughputOps = 0.0;
    double worstThroughputOps = 0.0;
    double meanCompletionRatio = 0.0;
    double medianCompletionRatio = 0.0;
    double bestCompletionRatio = 0.0;
    double worstCompletionRatio = 0.0;
    double meanAvgLatencyMs = 0.0;
    double medianAvgLatencyMs = 0.0;
    double bestAvgLatencyMs = 0.0;
    double worstAvgLatencyMs = 0.0;
    double meanP95LatencyMs = 0.0;
    double medianP95LatencyMs = 0.0;
    double bestP95LatencyMs = 0.0;
    double worstP95LatencyMs = 0.0;
    double meanP99LatencyMs = 0.0;
    double medianP99LatencyMs = 0.0;
    double bestP99LatencyMs = 0.0;
    double worstP99LatencyMs = 0.0;
    uint64_t totalClientErrors = 0;
};

inline BenchmarkSummary summarizeRuns(const std::vector<BenchmarkRunResult>& runs) {
    BenchmarkSummary summary;
    summary.rounds = runs.size();
    if (runs.empty()) {
        return summary;
    }

    std::vector<double> throughputs;
    std::vector<double> offeredRates;
    std::vector<double> completionRatios;
    std::vector<double> latencies;
    std::vector<double> p95Latencies;
    std::vector<double> p99Latencies;
    throughputs.reserve(runs.size());
    offeredRates.reserve(runs.size());
    completionRatios.reserve(runs.size());
    latencies.reserve(runs.size());
    p95Latencies.reserve(runs.size());
    p99Latencies.reserve(runs.size());

    double offeredRateSum = 0.0;
    double throughputSum = 0.0;
    double completionRatioSum = 0.0;
    double latencySum = 0.0;
    double p95LatencySum = 0.0;
    double p99LatencySum = 0.0;
    summary.bestOfferedOps = 0.0;
    summary.worstOfferedOps = 0.0;
    summary.bestThroughputOps = 0.0;
    summary.worstThroughputOps = 0.0;
    summary.bestCompletionRatio = 0.0;
    summary.worstCompletionRatio = 0.0;
    summary.bestAvgLatencyMs = 0.0;
    summary.worstAvgLatencyMs = 0.0;
    summary.bestP95LatencyMs = 0.0;
    summary.worstP95LatencyMs = 0.0;
    summary.bestP99LatencyMs = 0.0;
    summary.worstP99LatencyMs = 0.0;

    bool firstSuccess = true;
    for (const auto& run : runs) {
        summary.totalClientErrors += run.clientErrors;
        if (run.completed == 0) {
            continue;
        }

        const double offeredRate = run.offeredOpsPerSecond();
        const double throughput = run.throughputOpsPerSecond();
        const double completionRatio = run.completionRatio();
        offeredRates.push_back(offeredRate);
        const double latency = run.averageLatencyMs();
        completionRatios.push_back(completionRatio);
        const double p95Latency = run.p95LatencyMs();
        const double p99Latency = run.p99LatencyMs();
        throughputs.push_back(throughput);
        latencies.push_back(latency);
        p95Latencies.push_back(p95Latency);
        p99Latencies.push_back(p99Latency);
        offeredRateSum += offeredRate;
        throughputSum += throughput;
        completionRatioSum += completionRatio;
        latencySum += latency;
        p95LatencySum += p95Latency;
        p99LatencySum += p99Latency;
        ++summary.successfulRounds;

        if (firstSuccess) {
            summary.bestOfferedOps = offeredRate;
            summary.worstOfferedOps = offeredRate;
            summary.bestThroughputOps = throughput;
            summary.worstThroughputOps = throughput;
            summary.bestCompletionRatio = completionRatio;
            summary.worstCompletionRatio = completionRatio;
            summary.bestAvgLatencyMs = latency;
            summary.worstAvgLatencyMs = latency;
            summary.bestP95LatencyMs = p95Latency;
            summary.worstP95LatencyMs = p95Latency;
            summary.bestP99LatencyMs = p99Latency;
            summary.worstP99LatencyMs = p99Latency;
            firstSuccess = false;
        } else {
            summary.bestOfferedOps = (std::max)(summary.bestOfferedOps, offeredRate);
            summary.worstOfferedOps = (std::min)(summary.worstOfferedOps, offeredRate);
            summary.bestThroughputOps = (std::max)(summary.bestThroughputOps, throughput);
            summary.worstThroughputOps = (std::min)(summary.worstThroughputOps, throughput);
            summary.bestCompletionRatio = (std::max)(summary.bestCompletionRatio, completionRatio);
            summary.worstCompletionRatio = (std::min)(summary.worstCompletionRatio, completionRatio);
            summary.bestAvgLatencyMs = (std::min)(summary.bestAvgLatencyMs, latency);
            summary.worstAvgLatencyMs = (std::max)(summary.worstAvgLatencyMs, latency);
            summary.bestP95LatencyMs = (std::min)(summary.bestP95LatencyMs, p95Latency);
            summary.worstP95LatencyMs = (std::max)(summary.worstP95LatencyMs, p95Latency);
            summary.bestP99LatencyMs = (std::min)(summary.bestP99LatencyMs, p99Latency);
            summary.worstP99LatencyMs = (std::max)(summary.worstP99LatencyMs, p99Latency);
        }
    }

    if (summary.successfulRounds == 0) {
        return summary;
    }

    std::sort(offeredRates.begin(), offeredRates.end());
    std::sort(throughputs.begin(), throughputs.end());
    std::sort(completionRatios.begin(), completionRatios.end());
    std::sort(latencies.begin(), latencies.end());
    std::sort(p95Latencies.begin(), p95Latencies.end());
    std::sort(p99Latencies.begin(), p99Latencies.end());

    const size_t mid = throughputs.size() / 2;
    if ((throughputs.size() % 2) == 0) {
        summary.medianOfferedOps = (offeredRates[mid - 1] + offeredRates[mid]) / 2.0;
        summary.medianThroughputOps = (throughputs[mid - 1] + throughputs[mid]) / 2.0;
        summary.medianCompletionRatio = (completionRatios[mid - 1] + completionRatios[mid]) / 2.0;
        summary.medianAvgLatencyMs = (latencies[mid - 1] + latencies[mid]) / 2.0;
        summary.medianP95LatencyMs = (p95Latencies[mid - 1] + p95Latencies[mid]) / 2.0;
        summary.medianP99LatencyMs = (p99Latencies[mid - 1] + p99Latencies[mid]) / 2.0;
    } else {
        summary.medianOfferedOps = offeredRates[mid];
        summary.medianThroughputOps = throughputs[mid];
        summary.medianCompletionRatio = completionRatios[mid];
        summary.medianAvgLatencyMs = latencies[mid];
        summary.medianP95LatencyMs = p95Latencies[mid];
        summary.medianP99LatencyMs = p99Latencies[mid];
    }

    summary.meanOfferedOps = offeredRateSum / static_cast<double>(summary.successfulRounds);
    summary.meanThroughputOps = throughputSum / static_cast<double>(summary.successfulRounds);
    summary.meanCompletionRatio = completionRatioSum / static_cast<double>(summary.successfulRounds);
    summary.meanAvgLatencyMs = latencySum / static_cast<double>(summary.successfulRounds);
    summary.meanP95LatencyMs = p95LatencySum / static_cast<double>(summary.successfulRounds);
    summary.meanP99LatencyMs = p99LatencySum / static_cast<double>(summary.successfulRounds);
    return summary;
}

inline void printSummary(const BenchmarkSummary& summary) {
    std::cout << "\nSummary\n"
              << "  rounds         : " << summary.rounds << '\n'
              << "  successful     : " << summary.successfulRounds << '\n'
              << "  mean offered   : " << formatOpsRate(summary.meanOfferedOps) << '\n'
              << "  median offered : " << formatOpsRate(summary.medianOfferedOps) << '\n'
              << "  best offered   : " << formatOpsRate(summary.bestOfferedOps) << '\n'
              << "  worst offered  : " << formatOpsRate(summary.worstOfferedOps) << '\n'
              << "  mean thrpt     : " << formatOpsRate(summary.meanThroughputOps) << '\n'
              << "  median thrpt   : " << formatOpsRate(summary.medianThroughputOps) << '\n'
              << "  best thrpt     : " << formatOpsRate(summary.bestThroughputOps) << '\n'
              << "  worst thrpt    : " << formatOpsRate(summary.worstThroughputOps) << '\n'
              << "  mean complete  : " << std::fixed << std::setprecision(2)
              << (summary.meanCompletionRatio * 100.0) << "%\n"
              << "  median complete: " << std::fixed << std::setprecision(2)
              << (summary.medianCompletionRatio * 100.0) << "%\n"
              << "  best complete  : " << std::fixed << std::setprecision(2)
              << (summary.bestCompletionRatio * 100.0) << "%\n"
              << "  worst complete : " << std::fixed << std::setprecision(2)
              << (summary.worstCompletionRatio * 100.0) << "%\n";

    if (summary.successfulRounds != 0) {
        std::cout << "  mean avg rtt   : " << FastNet::BenchmarkUtils::formatLatency(summary.meanAvgLatencyMs) << '\n'
                  << "  median avg rtt : " << FastNet::BenchmarkUtils::formatLatency(summary.medianAvgLatencyMs) << '\n'
                  << "  best avg rtt   : " << FastNet::BenchmarkUtils::formatLatency(summary.bestAvgLatencyMs) << '\n'
                  << "  worst avg rtt  : " << FastNet::BenchmarkUtils::formatLatency(summary.worstAvgLatencyMs) << '\n'
                  << "  mean p95 rtt   : " << FastNet::BenchmarkUtils::formatLatency(summary.meanP95LatencyMs) << '\n'
                  << "  median p95 rtt : " << FastNet::BenchmarkUtils::formatLatency(summary.medianP95LatencyMs) << '\n'
                  << "  best p95 rtt   : " << FastNet::BenchmarkUtils::formatLatency(summary.bestP95LatencyMs) << '\n'
                  << "  worst p95 rtt  : " << FastNet::BenchmarkUtils::formatLatency(summary.worstP95LatencyMs) << '\n'
                  << "  mean p99 rtt   : " << FastNet::BenchmarkUtils::formatLatency(summary.meanP99LatencyMs) << '\n'
                  << "  median p99 rtt : " << FastNet::BenchmarkUtils::formatLatency(summary.medianP99LatencyMs) << '\n'
                  << "  best p99 rtt   : " << FastNet::BenchmarkUtils::formatLatency(summary.bestP99LatencyMs) << '\n'
                  << "  worst p99 rtt  : " << FastNet::BenchmarkUtils::formatLatency(summary.worstP99LatencyMs) << '\n';
    } else {
        std::cout << "  mean avg rtt   : n/a\n"
                  << "  median avg rtt : n/a\n"
                  << "  best avg rtt   : n/a\n"
                  << "  worst avg rtt  : n/a\n"
                  << "  mean p95 rtt   : n/a\n"
                  << "  median p95 rtt : n/a\n"
                  << "  best p95 rtt   : n/a\n"
                  << "  worst p95 rtt  : n/a\n"
                  << "  mean p99 rtt   : n/a\n"
                  << "  median p99 rtt : n/a\n"
                  << "  best p99 rtt   : n/a\n"
                  << "  worst p99 rtt  : n/a\n";
    }

    std::cout << "  total cli errs : " << summary.totalClientErrors << '\n';
}

} // namespace FastNetExamples
