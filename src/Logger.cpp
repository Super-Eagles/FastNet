/**
 * @file Logger.cpp
 * @brief FastNet logging implementation
 */
#include "Logger.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace FastNet {

namespace {

std::atomic<LogLevel> g_globalLogLevel{LogLevel::INFO};

std::string normalizeLogLevel(std::string_view text) {
    std::string normalized;
    normalized.reserve(text.size());
    for (char ch : text) {
        const unsigned char uch = static_cast<unsigned char>(ch);
        if (std::isalnum(uch)) {
            normalized.push_back(static_cast<char>(std::toupper(uch)));
        }
    }
    return normalized;
}

std::tm toLocalTime(std::time_t timestamp) {
    std::tm result{};
#ifdef _WIN32
    localtime_s(&result, &timestamp);
#else
    localtime_r(&timestamp, &result);
#endif
    return result;
}

const std::string& currentThreadIdString() {
    thread_local const std::string threadId = []() {
        std::ostringstream output;
        output << std::this_thread::get_id();
        return output.str();
    }();
    return threadId;
}

} // namespace

AsyncLogger& AsyncLogger::getInstance() {
    static AsyncLogger logger;
    return logger;
}

AsyncLogger::AsyncLogger() {
    currentBuffer_.reserve(4 * 1024 * 1024);
    backBuffer_.reserve(4 * 1024 * 1024);
}

AsyncLogger::~AsyncLogger() {
    shutdown();
}

void AsyncLogger::initialize(const std::string& filePath,
                             LogLevel level,
                             size_t maxFileSize,
                             bool mirrorToConsole) {
    const std::string desiredFilePath = filePath.empty() ? "fastnet.log" : filePath;

    std::unique_lock<std::mutex> lock(mutex_);
    if (running_.load(std::memory_order_acquire) && desiredFilePath == filePath_) {
        minLevel_.store(level, std::memory_order_release);
        mirrorToConsole_.store(mirrorToConsole, std::memory_order_release);
        maxFileSize_ = std::max<size_t>(maxFileSize, 1024 * 1024);
        return;
    }

    if (running_.load(std::memory_order_acquire)) {
        running_.store(false, std::memory_order_release);
        condition_.notify_one();
        lock.unlock();
        if (writeThread_.joinable()) {
            writeThread_.join();
        }
        lock.lock();
    } else if (writeThread_.joinable()) {
        lock.unlock();
        writeThread_.join();
        lock.lock();
    }

    filePath_ = desiredFilePath;
    minLevel_.store(level, std::memory_order_release);
    mirrorToConsole_.store(mirrorToConsole, std::memory_order_release);
    maxFileSize_ = std::max<size_t>(maxFileSize, 1024 * 1024);
    currentFileSize_ = 0;
    currentBuffer_.clear();
    backBuffer_.clear();

    openLogFile();
    running_.store(true, std::memory_order_release);
    writeThread_ = std::thread(&AsyncLogger::writerLoop, this);
}

void AsyncLogger::shutdown() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    condition_.notify_one();
    if (writeThread_.joinable()) {
        writeThread_.join();
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (logFile_.is_open()) {
        logFile_.flush();
        logFile_.close();
    }
    currentBuffer_.clear();
    backBuffer_.clear();
}

void AsyncLogger::flush() {
    if (!running_.load(std::memory_order_acquire)) {
        return;
    }
    condition_.notify_one();
}

void AsyncLogger::log(LogLevel level,
                      const char* file,
                      int line,
                      const char* func,
                      std::string_view message) {
    if (level < minLevel_.load(std::memory_order_acquire)) {
        return;
    }

    const std::string logLine = formatLogLine(level, file, line, func, message);
    if (!running_.load(std::memory_order_acquire)) {
        if (level >= LogLevel::ERROR_LVL) {
            std::cerr << logLine;
        } else {
            std::cout << logLine;
        }
        return;
    }

    bool shouldWake = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        currentBuffer_.append(logLine);
        shouldWake = currentBuffer_.size() >= flushThreshold_ || level >= LogLevel::ERROR_LVL;
    }

    if (shouldWake) {
        condition_.notify_one();
    }
}

void AsyncLogger::setLogLevel(LogLevel level) {
    minLevel_.store(level, std::memory_order_release);
}

LogLevel AsyncLogger::getLogLevel() const {
    return minLevel_.load(std::memory_order_acquire);
}

void AsyncLogger::setConsoleMirror(bool enabled) {
    mirrorToConsole_.store(enabled, std::memory_order_release);
}

bool AsyncLogger::isRunning() const {
    return running_.load(std::memory_order_acquire);
}

std::string AsyncLogger::formatLogLine(LogLevel level,
                                       const char* file,
                                       int line,
                                       const char* func,
                                       std::string_view message) const {
    // Extract basename from potentially long __FILE__ path.
    const char* baseName = file;
    if (file != nullptr) {
        for (const char* p = file; *p; ++p) {
            if (*p == '/' || *p == '\\') {
                baseName = p + 1;
            }
        }
    }

    const std::string timestamp = getCurrentTimestamp();
    const char* levelText = logLevelToString(level);
    const std::string& threadId = currentThreadIdString();

    // Convert line number to a stack buffer — avoids a std::string heap
    // allocation on every log() call (hot path).
    char lineBuf[12];
    const int lineLen = std::snprintf(lineBuf, sizeof(lineBuf), "%d", line);
    const std::string_view lineText{lineBuf, (lineLen > 0 ? static_cast<size_t>(lineLen) : 1)};

    // Pre-compute capacity to avoid reallocations inside the string builder.
    const size_t levelLen  = levelText  ? std::char_traits<char>::length(levelText)  : 7;
    const size_t baseLen   = baseName   ? std::char_traits<char>::length(baseName)   : 0;
    const size_t funcLen   = func       ? std::char_traits<char>::length(func)       : 0;
    const size_t capacity  = timestamp.size() + threadId.size()
                           + message.size() + lineText.size()
                           + levelLen + baseLen + funcLen + 24;

    std::string output;
    output.reserve(capacity);
    output.push_back('[');
    output.append(timestamp);
    output += "] [";
    output.append(levelText == nullptr ? "UNKNOWN" : levelText);
    output += "] [";
    output.append(threadId);
    output += "] ";
    output.append(message.data(), message.size());

    if (baseName != nullptr && func != nullptr) {
        output += " (";
        output.append(baseName);
        output.push_back(':');
        output.append(lineText.data(), lineText.size());
        output += " in ";
        output.append(func);
        output.push_back(')');
    }
    output.push_back('\n');
    return output;
}

void AsyncLogger::writerLoop() {
    while (true) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            condition_.wait_for(lock, flushInterval_, [this] {
                return !currentBuffer_.empty() || !running_.load(std::memory_order_acquire);
            });

            if (currentBuffer_.empty() && !running_.load(std::memory_order_acquire)) {
                break;
            }

            currentBuffer_.swap(backBuffer_);
        }

        if (!backBuffer_.empty()) {
            writeBuffer(backBuffer_);
            backBuffer_.clear();
        }
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!currentBuffer_.empty()) {
            currentBuffer_.swap(backBuffer_);
        }
    }
    if (!backBuffer_.empty()) {
        writeBuffer(backBuffer_);
        backBuffer_.clear();
    }
    if (logFile_.is_open()) {
        logFile_.flush();
    }
}

void AsyncLogger::writeBuffer(const std::string& buffer) {
    if (buffer.empty()) {
        return;
    }

    if (!logFile_.is_open()) {
        openLogFile();
    }

    if (logFile_.is_open()) {
        logFile_.write(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        currentFileSize_ += buffer.size();
        if (currentFileSize_ >= maxFileSize_) {
            rotateLogFile();
        }
    } else {
        std::cerr.write(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    }

    if (mirrorToConsole_.load(std::memory_order_acquire)) {
        std::cout.write(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    }
}

void AsyncLogger::openLogFile() {
    if (filePath_.empty()) {
        filePath_ = "fastnet.log";
    }

    const std::filesystem::path path(filePath_);
    if (path.has_parent_path()) {
        std::error_code mkdirError;
        std::filesystem::create_directories(path.parent_path(), mkdirError);
    }

    logFile_.close();
    logFile_.open(filePath_, std::ios::app | std::ios::out);
    if (!logFile_.is_open()) {
        std::cerr << "Failed to open log file: " << filePath_ << std::endl;
        currentFileSize_ = 0;
        return;
    }

    refreshCurrentFileSize();
}

void AsyncLogger::rotateLogFile() {
    if (!logFile_.is_open()) {
        openLogFile();
        return;
    }

    logFile_.flush();
    logFile_.close();

    const auto now = std::chrono::system_clock::now();
    const auto timeValue = std::chrono::system_clock::to_time_t(now);
    const std::tm localTime = toLocalTime(timeValue);
    const auto milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    const std::filesystem::path originalPath(filePath_);

    std::ostringstream rotatedName;
    rotatedName << originalPath.stem().string() << '.'
                << std::put_time(&localTime, "%Y%m%d_%H%M%S")
                << '_'
                << std::setfill('0')
                << std::setw(3)
                << milliseconds.count()
                << originalPath.extension().string();

    const std::filesystem::path rotatedPath =
        originalPath.has_parent_path()
            ? originalPath.parent_path() / rotatedName.str()
            : std::filesystem::path(rotatedName.str());

    std::error_code ec;
    std::filesystem::rename(originalPath, rotatedPath, ec);
    if (ec) {
        std::rename(filePath_.c_str(), rotatedPath.string().c_str());
    }

    currentFileSize_ = 0;
    openLogFile();
}

void AsyncLogger::refreshCurrentFileSize() {
    std::error_code ec;
    currentFileSize_ = std::filesystem::exists(filePath_, ec)
                           ? static_cast<size_t>(std::filesystem::file_size(filePath_, ec))
                           : 0;
    if (ec) {
        currentFileSize_ = 0;
    }
}

void setGlobalLogLevel(LogLevel level) {
    g_globalLogLevel.store(level, std::memory_order_release);
    AsyncLogger::getInstance().setLogLevel(level);
}

LogLevel getGlobalLogLevel() {
    return g_globalLogLevel.load(std::memory_order_acquire);
}

const char* logLevelToString(LogLevel level) {
    switch (level) {
        case LogLevel::TRACE:
            return "TRACE";
        case LogLevel::DEBUG:
            return "DEBUG";
        case LogLevel::INFO:
            return "INFO";
        case LogLevel::WARN_LVL:
            return "WARN";
        case LogLevel::ERROR_LVL:
            return "ERROR";
        case LogLevel::FATAL:
            return "FATAL";
        default:
            return "UNKNOWN";
    }
}

LogLevel logLevelFromString(std::string_view text, LogLevel fallback) {
    const std::string normalized = normalizeLogLevel(text);
    if (normalized == "TRACE") {
        return LogLevel::TRACE;
    }
    if (normalized == "DEBUG") {
        return LogLevel::DEBUG;
    }
    if (normalized == "INFO") {
        return LogLevel::INFO;
    }
    if (normalized == "WARN" || normalized == "WARNLVL") {
        return LogLevel::WARN_LVL;
    }
    if (normalized == "ERROR" || normalized == "ERRORLVL") {
        return LogLevel::ERROR_LVL;
    }
    if (normalized == "FATAL") {
        return LogLevel::FATAL;
    }
    return fallback;
}

std::string getCurrentTimestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto timeValue = std::chrono::system_clock::to_time_t(now);
    const auto milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    thread_local std::time_t cachedSecond = 0;
    thread_local std::array<char, 20> cachedPrefix{};

    if (cachedSecond != timeValue) {
        const std::tm localTime = toLocalTime(timeValue);
        std::strftime(cachedPrefix.data(), cachedPrefix.size(), "%Y-%m-%d %H:%M:%S", &localTime);
        cachedSecond = timeValue;
    }

    std::string output;
    output.reserve(23);
    output.append(cachedPrefix.data(), 19);
    output.push_back('.');
    const int millisecondValue = static_cast<int>(milliseconds.count());
    output.push_back(static_cast<char>('0' + (millisecondValue / 100) % 10));
    output.push_back(static_cast<char>('0' + (millisecondValue / 10) % 10));
    output.push_back(static_cast<char>('0' + millisecondValue % 10));
    return output;
}

void consoleLog(LogLevel level, std::string_view message) {
    if (level < getGlobalLogLevel()) {
        return;
    }

    // Build format line using string_view: no std::string construction for message.
    std::string line;
    line.reserve(message.size() + 48);
    line.push_back('[');
    line.append(getCurrentTimestamp());
    line += "] [";
    line.append(logLevelToString(level));
    line += "] ";
    line.append(message.data(), message.size());
    line.push_back('\n');

    if (level >= LogLevel::ERROR_LVL) {
        std::cerr << line;
    } else {
        std::cout << line;
    }
}

} // namespace FastNet
