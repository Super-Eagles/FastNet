/**
 * @file IoService.h
 * @brief FastNet IO Service - high-performance multi-threaded event loop
 */
#pragma once

#include "Config.h"

#include <cstddef>
#include <functional>
#include <memory>

namespace FastNet {

class EventPoller;
using Task = std::function<void()>;

class FASTNET_API IoService {
public:
    explicit IoService(size_t threadCount = 0);
    ~IoService() noexcept;

    IoService(const IoService&)            = delete;
    IoService& operator=(const IoService&) = delete;
    IoService(IoService&&)                 = delete;
    IoService& operator=(IoService&&)      = delete;

    // ── Lifecycle ─────────────────────────────────────────────────────────
    bool start();
    void stop();
    void join();

    // ── Task submission ───────────────────────────────────────────────────
    void post(const Task& task);
    void post(Task&& task);

    // ── Accessors ─────────────────────────────────────────────────────────
    EventPoller& getPoller();
    bool   isRunning()     const noexcept;
    size_t getThreadCount() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ── Global IoService ──────────────────────────────────────────────────────────
FASTNET_API void      configureGlobalIoService(size_t threadCount);
FASTNET_API IoService& getGlobalIoService();
FASTNET_API void      shutdownGlobalIoService();

} // namespace FastNet
