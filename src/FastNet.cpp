/**
 * @file FastNet.cpp
 * @brief FastNet library lifecycle implementation
 */
#include "FastNet.h"

#include "Configuration.h"
#include "IoService.h"
#include "Logger.h"
#include "PerformanceMonitor.h"
#include "SocketWrapper.h"
#include "Timer.h"
#include "WindowsIocpTransport.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace FastNet {

namespace {

std::mutex g_libraryMutex;
std::condition_variable g_libraryCondition;
size_t g_initializeRefCount = 0;
size_t g_initializedThreadCount = 0;

enum class LibraryState {
    Uninitialized,
    Initializing,
    Initialized,
    CleaningUp
};

LibraryState g_libraryState = LibraryState::Uninitialized;

size_t resolveRequestedThreadCount(size_t requestedThreadCount) {
    constexpr size_t kMaxThreadPoolSize = 1024;

    size_t effectiveThreadCount = requestedThreadCount;
    if (effectiveThreadCount == 0) {
        const int configuredThreadCount =
            getGlobalConfig().getInt(Configuration::Option::ThreadPoolSize, 0);
        if (configuredThreadCount > 0) {
            effectiveThreadCount = static_cast<size_t>(configuredThreadCount);
        }
    }

    if (effectiveThreadCount == 0) {
        effectiveThreadCount = std::thread::hardware_concurrency();
    }

    effectiveThreadCount = effectiveThreadCount == 0 ? 1 : effectiveThreadCount;
    return (std::min)(effectiveThreadCount, kMaxThreadPoolSize);
}

void applyRuntimeConfiguration() {
    auto& config = getGlobalConfig();
    const LogLevel logLevel = logLevelFromString(
        config.getString(Configuration::Option::LogLevel, "INFO"),
        LogLevel::INFO);

    setGlobalLogLevel(logLevel);
    AsyncLogger::getInstance().initialize(
        config.getString(Configuration::Option::LogFilePath, "fastnet.log"),
        logLevel);

    auto& performanceMonitor = getPerformanceMonitor();
    performanceMonitor.initialize(
        config.getBool(Configuration::Option::EnablePerformanceMonitoring, true));
}

void shutdownLibraryResources() {
    shutdownGlobalTimerManager();
    shutdownGlobalIoService();
    shutdownWindowsIocpTransport();
    getPerformanceMonitor().shutdown();
    AsyncLogger::getInstance().shutdown();
    SocketWrapper::cleanupSocketLibrary();
}

} // namespace

ConnectionId generateConnectionId() {
    static std::atomic<ConnectionId> counter{1};
    return counter.fetch_add(1, std::memory_order_relaxed);
}

ErrorCode initialize(size_t threadCount) {
    const size_t requestedThreadCount = resolveRequestedThreadCount(threadCount);
    std::unique_lock<std::mutex> lock(g_libraryMutex);
    g_libraryCondition.wait(lock, []() {
        return g_libraryState != LibraryState::Initializing &&
               g_libraryState != LibraryState::CleaningUp;
    });

    if (g_libraryState == LibraryState::Initialized) {
        if (threadCount != 0 && g_initializedThreadCount != requestedThreadCount) {
            return ErrorCode::AlreadyRunning;
        }

        try {
            applyRuntimeConfiguration();
            ++g_initializeRefCount;
            return ErrorCode::Success;
        } catch (...) {
            return ErrorCode::UnknownError;
        }
    }

    g_libraryState = LibraryState::Initializing;
    lock.unlock();

    bool socketLibraryReady = false;
    ErrorCode result = ErrorCode::UnknownError;
    size_t actualThreadCount = 0;

    try {
        const Error socketInitResult = SocketWrapper::initializeSocketLibrary();
        if (socketInitResult.isFailure()) {
            result = socketInitResult.getCode();
        } else {
            socketLibraryReady = true;

            configureGlobalIoService(requestedThreadCount);
            auto& ioService = getGlobalIoService();
            if (!ioService.start()) {
                shutdownGlobalIoService();
                SocketWrapper::cleanupSocketLibrary();
                socketLibraryReady = false;
            } else {
                getGlobalTimerManager();
                applyRuntimeConfiguration();

                actualThreadCount = ioService.getThreadCount();
                result = ErrorCode::Success;
            }
        }
    } catch (...) {
        getPerformanceMonitor().shutdown();
        AsyncLogger::getInstance().shutdown();
        shutdownGlobalTimerManager();
        shutdownGlobalIoService();
        if (socketLibraryReady) {
            SocketWrapper::cleanupSocketLibrary();
            socketLibraryReady = false;
        }
        result = ErrorCode::UnknownError;
    }

    lock.lock();
    if (result == ErrorCode::Success) {
        g_libraryState = LibraryState::Initialized;
        g_initializeRefCount = 1;
        g_initializedThreadCount = actualThreadCount;
    } else {
        g_libraryState = LibraryState::Uninitialized;
        g_initializeRefCount = 0;
        g_initializedThreadCount = 0;
    }
    lock.unlock();
    g_libraryCondition.notify_all();
    return result;
}

void cleanup() {
    std::unique_lock<std::mutex> lock(g_libraryMutex);
    g_libraryCondition.wait(lock, []() {
        return g_libraryState != LibraryState::Initializing &&
               g_libraryState != LibraryState::CleaningUp;
    });

    if (g_libraryState != LibraryState::Initialized || g_initializeRefCount == 0) {
        return;
    }

    --g_initializeRefCount;
    if (g_initializeRefCount != 0) {
        return;
    }

    g_libraryState = LibraryState::CleaningUp;
    g_initializedThreadCount = 0;
    lock.unlock();

    shutdownLibraryResources();

    lock.lock();
    g_libraryState = LibraryState::Uninitialized;
    lock.unlock();
    g_libraryCondition.notify_all();
}

bool isInitialized() {
    std::lock_guard<std::mutex> lock(g_libraryMutex);
    return g_libraryState == LibraryState::Initialized;
}

} // namespace FastNet
