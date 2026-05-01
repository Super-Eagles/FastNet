/**
 * @file BenchmarkUtils.cpp
 * @brief FastNet benchmark helper utilities
 */
#include "BenchmarkUtils.h"

#include <array>
#include <iomanip>
#include <sstream>

namespace FastNet {

namespace {

std::string formatScaledValue(double value,
                              const std::array<const char*, 5>& units,
                              double step) {
    size_t unitIndex = 0;
    while (unitIndex + 1 < units.size() && value >= step) {
        value /= step;
        ++unitIndex;
    }

    std::ostringstream output;
    output << std::fixed << std::setprecision(value >= 100.0 ? 1 : 2) << value << ' ' << units[unitIndex];
    return output.str();
}

} // namespace

double BenchmarkUtils::getCurrentTime() {
    const auto duration = now().time_since_epoch();
    return std::chrono::duration<double>(duration).count();
}

std::chrono::steady_clock::time_point BenchmarkUtils::now() noexcept {
    return std::chrono::steady_clock::now();
}

std::string BenchmarkUtils::formatBandwidth(uint64_t bytes, double seconds) {
    if (seconds <= 0.0) {
        return "0 B/s";
    }

    static constexpr std::array<const char*, 5> kUnits{{"B/s", "KB/s", "MB/s", "GB/s", "TB/s"}};
    return formatScaledValue(static_cast<double>(bytes) / seconds, kUnits, 1024.0);
}

std::string BenchmarkUtils::formatLatency(double milliseconds) {
    if (milliseconds < 0.0) {
        milliseconds = 0.0;
    }

    if (milliseconds < 1.0) {
        std::ostringstream output;
        output << std::fixed << std::setprecision(2) << (milliseconds * 1000.0) << " us";
        return output.str();
    }

    return formatDuration(milliseconds);
}

std::string BenchmarkUtils::formatBytes(uint64_t bytes) {
    static constexpr std::array<const char*, 5> kUnits{{"B", "KB", "MB", "GB", "TB"}};
    return formatScaledValue(static_cast<double>(bytes), kUnits, 1024.0);
}

std::string BenchmarkUtils::formatOpsPerSecond(uint64_t operations, double seconds) {
    if (seconds <= 0.0) {
        return "0 ops/s";
    }

    static constexpr std::array<const char*, 5> kUnits{{"ops/s", "Kops/s", "Mops/s", "Gops/s", "Tops/s"}};
    return formatScaledValue(static_cast<double>(operations) / seconds, kUnits, 1000.0);
}

std::string BenchmarkUtils::formatDuration(double milliseconds) {
    if (milliseconds < 0.0) {
        milliseconds = 0.0;
    }

    std::ostringstream output;
    if (milliseconds < 1000.0) {
        output << std::fixed << std::setprecision(2) << milliseconds << " ms";
        return output.str();
    }

    const double seconds = milliseconds / 1000.0;
    if (seconds < 60.0) {
        output << std::fixed << std::setprecision(2) << seconds << " s";
        return output.str();
    }

    const double minutes = seconds / 60.0;
    output << std::fixed << std::setprecision(2) << minutes << " min";
    return output.str();
}

} // namespace FastNet
