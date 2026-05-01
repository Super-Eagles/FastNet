/**
 * @file Configuration.h
 * @brief FastNet configuration management
 */
#pragma once

#include "Config.h"

#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <shared_mutex>
#include <string>
#include <string_view>

namespace FastNet {

class FASTNET_API Configuration {
public:
    enum class Option {
        MaxConnections,
        ConnectionTimeout,
        ReadTimeout,
        WriteTimeout,
        ThreadPoolSize,
        ThreadIdleTimeout,
        BufferSize,
        MaxBufferSize,
        LogLevel,
        LogFilePath,
        EnableSSL,
        CertificateFile,
        PrivateKeyFile,
        CAFile,
        VerifyPeer,
        MaxHttpRequestSize,
        MaxHttpResponseSize,
        EnableCompression,
        CompressionLevel,
        WebSocketPingInterval,
        WebSocketPongTimeout,
        EnablePerformanceMonitoring,
        PerformanceReportInterval,
        LoadBalancingStrategy,
        HealthCheckInterval,
        CircuitBreakerEnabled,
        FailureThreshold,
        RecoveryTimeout,
        HalfOpenAttempts,
    };

    enum class MergeMode {
        OverrideExisting,
        PreserveExisting
    };

    Configuration();
    Configuration(const Configuration& other);
    ~Configuration();

    bool loadFromFile(const std::string& filename);
    bool saveToFile(const std::string& filename) const;
    void loadFromEnvironment();
    void loadFromEnvironment(const std::string& prefix);

    std::string getString(Option option, const std::string& defaultValue = "") const;
    int getInt(Option option, int defaultValue = 0) const;
    bool getBool(Option option, bool defaultValue = false) const;
    double getDouble(Option option, double defaultValue = 0.0) const;

    void set(Option option, const std::string& value);
    void set(Option option, std::string_view value);
    void set(Option option, const char* value);
    void set(Option option, int value);
    void set(Option option, bool value);
    void set(Option option, double value);

    bool has(Option option) const;
    bool validate() const;
    bool validateOption(Option option, const std::string& value) const;

    void remove(Option option);
    void clear();

    std::set<std::string> getAllKeys() const;
    std::map<Option, std::string> snapshot() const;
    void merge(const Configuration& other);
    void merge(const Configuration& other, MergeMode mode);

    Configuration& operator=(const Configuration& other);

private:
    static std::string optionToString(Option option);
    static std::optional<Option> stringToOption(std::string_view text);

    void populateDefaultsLocked();
    void setDefaults();
    bool parseConfigFile(const std::string& content, std::map<Option, std::string>& parsedOptions) const;
    std::string serialize() const;

    mutable std::shared_mutex mutex_;
    std::map<Option, std::string> options_;
    std::set<Option> explicitOptions_;
};

FASTNET_API Configuration& getGlobalConfig();

} // namespace FastNet
