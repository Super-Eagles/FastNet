/**
 * @file Logger.h
 * @brief FastNet logging API
 */
#pragma once

#include "Config.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <condition_variable>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace FastNet {

enum class LogLevel {
    TRACE = 0,
    DEBUG = 1,
    INFO = 2,
    WARN_LVL = 3,
    ERROR_LVL = 4,
    FATAL = 5
};

class FASTNET_API AsyncLogger {
public:
    static AsyncLogger& getInstance();

    AsyncLogger(const AsyncLogger&) = delete;
    AsyncLogger& operator=(const AsyncLogger&) = delete;

    void initialize(const std::string& filePath = "fastnet.log",
                    LogLevel level = LogLevel::INFO,
                    size_t maxFileSize = 100 * 1024 * 1024,
                    bool mirrorToConsole = false);
    void shutdown();
    void flush();

    void log(LogLevel level,
             const char* file,
             int line,
             const char* func,
             std::string_view message);

    void setLogLevel(LogLevel level);
    LogLevel getLogLevel() const;

    void setConsoleMirror(bool enabled);
    bool isRunning() const;

private:
    AsyncLogger();
    ~AsyncLogger();

    struct Impl;
    std::unique_ptr<Impl> pImpl_;
};

FASTNET_API void setGlobalLogLevel(LogLevel level);
FASTNET_API LogLevel getGlobalLogLevel();
FASTNET_API const char* logLevelToString(LogLevel level);
FASTNET_API LogLevel logLevelFromString(std::string_view text,
                                        LogLevel fallback = LogLevel::INFO);
FASTNET_API std::string getCurrentTimestamp();
FASTNET_API void consoleLog(LogLevel level, std::string_view message);

} // namespace FastNet

#define LOG_TRACE(msg) \
    ::FastNet::AsyncLogger::getInstance().log(::FastNet::LogLevel::TRACE, __FILE__, __LINE__, __func__, (msg))
#define LOG_DEBUG(msg) \
    ::FastNet::AsyncLogger::getInstance().log(::FastNet::LogLevel::DEBUG, __FILE__, __LINE__, __func__, (msg))
#define LOG_INFO(msg) \
    ::FastNet::AsyncLogger::getInstance().log(::FastNet::LogLevel::INFO, __FILE__, __LINE__, __func__, (msg))
#define LOG_WARN(msg) \
    ::FastNet::AsyncLogger::getInstance().log(::FastNet::LogLevel::WARN_LVL, __FILE__, __LINE__, __func__, (msg))
#define LOG_ERROR(msg) \
    ::FastNet::AsyncLogger::getInstance().log(::FastNet::LogLevel::ERROR_LVL, __FILE__, __LINE__, __func__, (msg))
#define LOG_FATAL(msg) \
    ::FastNet::AsyncLogger::getInstance().log(::FastNet::LogLevel::FATAL, __FILE__, __LINE__, __func__, (msg))

#define FASTNET_LOG_TRACE(msg) LOG_TRACE(msg)
#define FASTNET_LOG_DEBUG(msg) LOG_DEBUG(msg)
#define FASTNET_LOG_INFO(msg) LOG_INFO(msg)
#define FASTNET_LOG_WARN(msg) LOG_WARN(msg)
#define FASTNET_LOG_ERROR(msg) LOG_ERROR(msg)
#define FASTNET_LOG_FATAL(msg) LOG_FATAL(msg)
