/**
 * @file Configuration.cpp
 * @brief FastNet configuration management
 */
#include "Configuration.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <sstream>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace FastNet {

namespace {

using Option = Configuration::Option;

struct OptionEntry {
    Option option;
    const char* name;
};

constexpr std::array<OptionEntry, 29> kOptionEntries{{
    {Option::MaxConnections, "MaxConnections"},
    {Option::ConnectionTimeout, "ConnectionTimeout"},
    {Option::ReadTimeout, "ReadTimeout"},
    {Option::WriteTimeout, "WriteTimeout"},
    {Option::ThreadPoolSize, "ThreadPoolSize"},
    {Option::ThreadIdleTimeout, "ThreadIdleTimeout"},
    {Option::BufferSize, "BufferSize"},
    {Option::MaxBufferSize, "MaxBufferSize"},
    {Option::LogLevel, "LogLevel"},
    {Option::LogFilePath, "LogFilePath"},
    {Option::EnableSSL, "EnableSSL"},
    {Option::CertificateFile, "CertificateFile"},
    {Option::PrivateKeyFile, "PrivateKeyFile"},
    {Option::CAFile, "CAFile"},
    {Option::VerifyPeer, "VerifyPeer"},
    {Option::MaxHttpRequestSize, "MaxHttpRequestSize"},
    {Option::MaxHttpResponseSize, "MaxHttpResponseSize"},
    {Option::EnableCompression, "EnableCompression"},
    {Option::CompressionLevel, "CompressionLevel"},
    {Option::WebSocketPingInterval, "WebSocketPingInterval"},
    {Option::WebSocketPongTimeout, "WebSocketPongTimeout"},
    {Option::EnablePerformanceMonitoring, "EnablePerformanceMonitoring"},
    {Option::PerformanceReportInterval, "PerformanceReportInterval"},
    {Option::LoadBalancingStrategy, "LoadBalancingStrategy"},
    {Option::HealthCheckInterval, "HealthCheckInterval"},
    {Option::CircuitBreakerEnabled, "CircuitBreakerEnabled"},
    {Option::FailureThreshold, "FailureThreshold"},
    {Option::RecoveryTimeout, "RecoveryTimeout"},
    {Option::HalfOpenAttempts, "HalfOpenAttempts"},
}};

constexpr std::array<std::pair<Option, const char*>, 29> kDefaultOptionValues{{
    {Option::MaxConnections, "1000"},
    {Option::ConnectionTimeout, "30000"},
    {Option::ReadTimeout, "30000"},
    {Option::WriteTimeout, "30000"},
    {Option::ThreadPoolSize, "4"},
    {Option::ThreadIdleTimeout, "60000"},
    {Option::BufferSize, "4096"},
    {Option::MaxBufferSize, "65536"},
    {Option::LogLevel, "INFO"},
    {Option::LogFilePath, "fastnet.log"},
    {Option::EnableSSL, "false"},
    {Option::CertificateFile, ""},
    {Option::PrivateKeyFile, ""},
    {Option::CAFile, ""},
    {Option::VerifyPeer, "true"},
    {Option::MaxHttpRequestSize, "1048576"},
    {Option::MaxHttpResponseSize, "1048576"},
    {Option::EnableCompression, "true"},
    {Option::CompressionLevel, "6"},
    {Option::WebSocketPingInterval, "30000"},
    {Option::WebSocketPongTimeout, "10000"},
    {Option::EnablePerformanceMonitoring, "true"},
    {Option::PerformanceReportInterval, "5000"},
    {Option::LoadBalancingStrategy, "RoundRobin"},
    {Option::HealthCheckInterval, "30000"},
    {Option::CircuitBreakerEnabled, "true"},
    {Option::FailureThreshold, "5"},
    {Option::RecoveryTimeout, "60000"},
    {Option::HalfOpenAttempts, "3"},
}};

std::optional<std::string> getDefaultOptionValue(Option option) {
    const auto it = std::find_if(
        kDefaultOptionValues.begin(),
        kDefaultOptionValues.end(),
        [option](const auto& entry) { return entry.first == option; });
    if (it == kDefaultOptionValues.end()) {
        return std::nullopt;
    }
    return std::string(it->second);
}

std::string trimCopy(const std::string& value) {
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return {};
    }
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

std::string toLowerCopy(std::string value) {
    std::transform(value.begin(),
                   value.end(),
                   value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

std::string toUpperCopy(std::string value) {
    std::transform(value.begin(),
                   value.end(),
                   value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    return value;
}

std::string canonicalizeOptionName(std::string_view text) {
    std::string normalized;
    normalized.reserve(text.size());
    for (char ch : text) {
        const unsigned char uch = static_cast<unsigned char>(ch);
        if (std::isalnum(uch)) {
            normalized.push_back(static_cast<char>(std::tolower(uch)));
        }
    }

    static const std::string prefix = "fastnet";
    if (normalized.compare(0, prefix.size(), prefix) == 0) {
        normalized.erase(0, prefix.size());
    }
    return normalized;
}

std::optional<bool> parseBoolValue(std::string_view value) {
    const std::string normalized = toLowerCopy(trimCopy(std::string(value)));
    if (normalized == "true" || normalized == "1" || normalized == "yes" || normalized == "on") {
        return true;
    }
    if (normalized == "false" || normalized == "0" || normalized == "no" || normalized == "off") {
        return false;
    }
    return std::nullopt;
}

bool parseIntegerStrict(const std::string& text, long long& result) {
    const std::string trimmed = trimCopy(text);
    if (trimmed.empty()) {
        return false;
    }

    try {
        size_t consumed = 0;
        const long long value = std::stoll(trimmed, &consumed, 10);
        if (consumed != trimmed.size()) {
            return false;
        }
        result = value;
        return true;
    } catch (...) {
        return false;
    }
}

bool parseDoubleStrict(const std::string& text, double& result) {
    const std::string trimmed = trimCopy(text);
    if (trimmed.empty()) {
        return false;
    }

    try {
        size_t consumed = 0;
        const double value = std::stod(trimmed, &consumed);
        if (consumed != trimmed.size()) {
            return false;
        }
        result = value;
        return true;
    } catch (...) {
        return false;
    }
}

std::string toEnvironmentKey(std::string_view optionName) {
    std::string result;
    result.reserve(optionName.size() + 8);
    for (size_t i = 0; i < optionName.size(); ++i) {
        const char current = optionName[i];
        const bool currentUpper = std::isupper(static_cast<unsigned char>(current)) != 0;
        const bool previousLower =
            i > 0 && std::islower(static_cast<unsigned char>(optionName[i - 1])) != 0;
        const bool nextLower =
            i + 1 < optionName.size() &&
            std::islower(static_cast<unsigned char>(optionName[i + 1])) != 0;
        if (i > 0 && currentUpper && (previousLower || nextLower)) {
            result.push_back('_');
        }
        result.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(current))));
    }
    return result;
}

std::optional<std::string> readEnvironmentVariable(const std::string& key) {
#ifdef _WIN32
    const DWORD size = GetEnvironmentVariableA(key.c_str(), nullptr, 0);
    if (size == 0) {
        return std::nullopt;
    }

    std::vector<char> buffer(size);
    if (GetEnvironmentVariableA(key.c_str(), buffer.data(), size) == 0) {
        return std::nullopt;
    }
    return std::string(buffer.data(), buffer.data() + size - 1);
#else
    const char* value = std::getenv(key.c_str());
    if (value == nullptr) {
        return std::nullopt;
    }
    return std::string(value);
#endif
}

std::string stripInlineComment(const std::string& line) {
    bool inQuotes = false;
    for (size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '"') {
            inQuotes = !inQuotes;
            continue;
        }

        if (inQuotes) {
            continue;
        }

        if ((line[i] == '#' || line[i] == ';') &&
            (i == 0 || std::isspace(static_cast<unsigned char>(line[i - 1])) != 0)) {
            return trimCopy(line.substr(0, i));
        }
    }
    return trimCopy(line);
}

} // namespace

Configuration::Configuration() {
    setDefaults();
}

Configuration::Configuration(const Configuration& other)
{
    std::shared_lock<std::shared_mutex> lock(other.mutex_);
    options_ = other.options_;
    explicitOptions_ = other.explicitOptions_;
}

Configuration::~Configuration() = default;

bool Configuration::loadFromFile(const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        return false;
    }

    std::ostringstream buffer;
    buffer << file.rdbuf();
    std::map<Option, std::string> parsedOptions;
    if (!parseConfigFile(buffer.str(), parsedOptions)) {
        return false;
    }

    std::unique_lock<std::shared_mutex> lock(mutex_);
    for (const auto& entry : parsedOptions) {
        options_[entry.first] = entry.second;
        explicitOptions_.insert(entry.first);
    }
    return true;
}

bool Configuration::saveToFile(const std::string& filename) const {
    std::ofstream file(filename);
    if (!file.is_open()) {
        return false;
    }

    file << serialize();
    return static_cast<bool>(file);
}

void Configuration::loadFromEnvironment() {
    loadFromEnvironment("FASTNET_");
}

void Configuration::loadFromEnvironment(const std::string& prefix) {
    std::map<Option, std::string> parsedOptions;
    for (const auto& entry : kOptionEntries) {
        const std::string optionName(entry.name);
        const std::string upperName = toUpperCopy(optionName);
        const std::string snakeName = toEnvironmentKey(optionName);

        std::vector<std::string> candidates;
        candidates.reserve(4);
        if (!prefix.empty()) {
            candidates.push_back(prefix + snakeName);
            candidates.push_back(prefix + upperName);
        }
        candidates.push_back(snakeName);
        candidates.push_back(upperName);

        for (const auto& key : candidates) {
            const auto value = readEnvironmentVariable(key);
            if (!value.has_value()) {
                continue;
            }

            const std::string trimmedValue = trimCopy(*value);
            if (validateOption(entry.option, trimmedValue)) {
                parsedOptions[entry.option] = trimmedValue;
            }
            break;
        }
    }

    std::unique_lock<std::shared_mutex> lock(mutex_);
    for (const auto& entry : parsedOptions) {
        options_[entry.first] = entry.second;
        explicitOptions_.insert(entry.first);
    }
}

std::string Configuration::getString(Option option, const std::string& defaultValue) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    const auto it = options_.find(option);
    return it == options_.end() ? defaultValue : it->second;
}

int Configuration::getInt(Option option, int defaultValue) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    const auto it = options_.find(option);
    if (it == options_.end()) {
        return defaultValue;
    }

    long long value = 0;
    if (!parseIntegerStrict(it->second, value)) {
        return defaultValue;
    }
    if (value < (std::numeric_limits<int>::min)() || value > (std::numeric_limits<int>::max)()) {
        return defaultValue;
    }
    return static_cast<int>(value);
}

bool Configuration::getBool(Option option, bool defaultValue) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    const auto it = options_.find(option);
    if (it == options_.end()) {
        return defaultValue;
    }

    const auto parsed = parseBoolValue(it->second);
    return parsed.has_value() ? *parsed : defaultValue;
}

double Configuration::getDouble(Option option, double defaultValue) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    const auto it = options_.find(option);
    if (it == options_.end()) {
        return defaultValue;
    }

    double value = 0.0;
    return parseDoubleStrict(it->second, value) ? value : defaultValue;
}

void Configuration::set(Option option, const std::string& value) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    options_[option] = trimCopy(value);
    explicitOptions_.insert(option);
}

void Configuration::set(Option option, std::string_view value) {
    set(option, std::string(value));
}

void Configuration::set(Option option, const char* value) {
    set(option, value == nullptr ? std::string() : std::string(value));
}

void Configuration::set(Option option, int value) {
    set(option, std::to_string(value));
}

void Configuration::set(Option option, bool value) {
    set(option, std::string(value ? "true" : "false"));
}

void Configuration::set(Option option, double value) {
    set(option, std::to_string(value));
}

bool Configuration::has(Option option) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return explicitOptions_.find(option) != explicitOptions_.end();
}

bool Configuration::validate() const {
    const auto values = snapshot();
    for (const auto& entry : values) {
        if (!validateOption(entry.first, entry.second)) {
            return false;
        }
    }
    return true;
}

bool Configuration::validateOption(Option option, const std::string& value) const {
    long long integerValue = 0;

    switch (option) {
        case Option::MaxConnections:
            return parseIntegerStrict(value, integerValue) && integerValue > 0 && integerValue <= 100000;
        case Option::ConnectionTimeout:
        case Option::ReadTimeout:
        case Option::WriteTimeout:
        case Option::ThreadIdleTimeout:
        case Option::HealthCheckInterval:
        case Option::RecoveryTimeout:
        case Option::WebSocketPingInterval:
        case Option::WebSocketPongTimeout:
        case Option::PerformanceReportInterval:
            return parseIntegerStrict(value, integerValue) && integerValue >= 0 && integerValue <= 3600000;
        case Option::ThreadPoolSize:
            return parseIntegerStrict(value, integerValue) && integerValue > 0 && integerValue <= 256;
        case Option::BufferSize:
        case Option::MaxBufferSize:
            return parseIntegerStrict(value, integerValue) && integerValue >= 1024 && integerValue <= 10485760;
        case Option::MaxHttpRequestSize:
        case Option::MaxHttpResponseSize:
            return parseIntegerStrict(value, integerValue) && integerValue >= 1024 && integerValue <= 104857600;
        case Option::CompressionLevel:
            return parseIntegerStrict(value, integerValue) && integerValue >= 0 && integerValue <= 9;
        case Option::FailureThreshold:
        case Option::HalfOpenAttempts:
            return parseIntegerStrict(value, integerValue) && integerValue > 0 && integerValue <= 100;
        case Option::EnableSSL:
        case Option::VerifyPeer:
        case Option::EnableCompression:
        case Option::EnablePerformanceMonitoring:
        case Option::CircuitBreakerEnabled:
            return parseBoolValue(value).has_value();
        case Option::LogLevel: {
            const std::string normalized = toUpperCopy(trimCopy(value));
            return normalized == "TRACE" || normalized == "DEBUG" || normalized == "INFO" ||
                   normalized == "WARN" || normalized == "ERROR" || normalized == "FATAL";
        }
        case Option::LoadBalancingStrategy: {
            const std::string normalized = canonicalizeOptionName(value);
            return normalized == "roundrobin" || normalized == "random" ||
                   normalized == "leastconnections" || normalized == "weightedroundrobin" ||
                   normalized == "iphash";
        }
        case Option::LogFilePath:
        case Option::CertificateFile:
        case Option::PrivateKeyFile:
        case Option::CAFile:
            return true;
        default:
            return true;
    }
}

void Configuration::remove(Option option) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    explicitOptions_.erase(option);
    const auto defaultValue = getDefaultOptionValue(option);
    if (defaultValue.has_value()) {
        options_[option] = *defaultValue;
    } else {
        options_.erase(option);
    }
}

void Configuration::clear() {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    populateDefaultsLocked();
}

std::set<std::string> Configuration::getAllKeys() const {
    std::set<std::string> keys;
    const auto values = snapshot();
    for (const auto& entry : values) {
        keys.insert(optionToString(entry.first));
    }
    return keys;
}

std::map<Option, std::string> Configuration::snapshot() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return options_;
}

void Configuration::merge(const Configuration& other) {
    merge(other, MergeMode::OverrideExisting);
}

void Configuration::merge(const Configuration& other, MergeMode mode) {
    std::map<Option, std::string> otherValues;
    std::set<Option> otherExplicit;
    {
        std::shared_lock<std::shared_mutex> lock(other.mutex_);
        otherValues = other.options_;
        otherExplicit = other.explicitOptions_;
    }

    std::unique_lock<std::shared_mutex> lock(mutex_);
    for (const auto& option : otherExplicit) {
        const auto entry = otherValues.find(option);
        if (entry == otherValues.end()) {
            continue;
        }
        if (!validateOption(entry->first, entry->second)) {
            continue;
        }
        if (mode == MergeMode::PreserveExisting &&
            explicitOptions_.find(entry->first) != explicitOptions_.end()) {
            continue;
        }
        options_[entry->first] = entry->second;
        explicitOptions_.insert(entry->first);
    }
}

Configuration& Configuration::operator=(const Configuration& other) {
    if (this == &other) {
        return *this;
    }

    std::map<Option, std::string> values;
    std::set<Option> explicitValues;
    {
        std::shared_lock<std::shared_mutex> otherLock(other.mutex_);
        values = other.options_;
        explicitValues = other.explicitOptions_;
    }

    std::unique_lock<std::shared_mutex> lock(mutex_);
    options_ = values;
    explicitOptions_ = explicitValues;
    return *this;
}

std::string Configuration::optionToString(Option option) {
    const auto it = std::find_if(
        kOptionEntries.begin(), kOptionEntries.end(), [option](const OptionEntry& entry) {
            return entry.option == option;
        });
    return it == kOptionEntries.end() ? std::string() : std::string(it->name);
}

std::optional<Configuration::Option> Configuration::stringToOption(std::string_view text) {
    const std::string normalized = canonicalizeOptionName(text);
    if (normalized.empty()) {
        return std::nullopt;
    }

    const auto it = std::find_if(
        kOptionEntries.begin(), kOptionEntries.end(), [&normalized](const OptionEntry& entry) {
            return canonicalizeOptionName(entry.name) == normalized;
        });
    if (it == kOptionEntries.end()) {
        return std::nullopt;
    }
    return it->option;
}

void Configuration::populateDefaultsLocked() {
    options_.clear();
    explicitOptions_.clear();
    for (const auto& entry : kDefaultOptionValues) {
        options_[entry.first] = entry.second;
    }
}

void Configuration::setDefaults() {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    populateDefaultsLocked();
}

bool Configuration::parseConfigFile(const std::string& content,
                                    std::map<Option, std::string>& parsedOptions) const {
    std::istringstream input(content);
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && static_cast<unsigned char>(line[0]) == 0xEF) {
            if (line.size() >= 3 &&
                static_cast<unsigned char>(line[1]) == 0xBB &&
                static_cast<unsigned char>(line[2]) == 0xBF) {
                line.erase(0, 3);
            }
        }

        const std::string stripped = stripInlineComment(line);
        if (stripped.empty()) {
            continue;
        }

        const size_t separator = stripped.find('=');
        if (separator == std::string::npos) {
            continue;
        }

        const std::string key = trimCopy(stripped.substr(0, separator));
        const std::string value = trimCopy(stripped.substr(separator + 1));
        const auto option = stringToOption(key);
        if (!option.has_value()) {
            continue;
        }

        if (!validateOption(*option, value)) {
            return false;
        }

        parsedOptions[*option] = value;
    }

    return true;
}

std::string Configuration::serialize() const {
    const auto values = snapshot();
    std::ostringstream output;
    for (const auto& entry : values) {
        output << optionToString(entry.first) << '=' << entry.second << '\n';
    }
    return output.str();
}

Configuration& getGlobalConfig() {
    static Configuration config;
    return config;
}

} // namespace FastNet
