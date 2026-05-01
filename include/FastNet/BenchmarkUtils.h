/**
 * @file BenchmarkUtils.h
 * @brief FastNet benchmark helper utilities
 */
#pragma once

#include "Config.h"

#include <chrono>
#include <cstdint>
#include <string>

namespace FastNet {

class FASTNET_API BenchmarkUtils {
public:
    static double getCurrentTime();
    static std::chrono::steady_clock::time_point now() noexcept;

    static std::string formatBandwidth(uint64_t bytes, double seconds);
    static std::string formatLatency(double milliseconds);
    static std::string formatBytes(uint64_t bytes);
    static std::string formatOpsPerSecond(uint64_t operations, double seconds);
    static std::string formatDuration(double milliseconds);
};

} // namespace FastNet
