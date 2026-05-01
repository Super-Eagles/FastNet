#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <iomanip>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

struct RunnerOptions {
    enum class Profile {
        Smoke,
        Standard,
        High,
        Soak
    };

    Profile profile = Profile::Smoke;
    std::filesystem::path binDir;
    std::filesystem::path outputPath;
    bool includeTls = false;
    size_t loopbackClients = 4;
    size_t loopbackMaxInflight = 1;
    size_t loopbackTargetQps = 0;
    size_t connectClients = 64;
    size_t payloadBytes = 256;
    uint32_t loopbackWarmupSeconds = 1;
    uint32_t loopbackDurationSeconds = 1;
    uint32_t loopbackRounds = 1;
    uint32_t connectWarmupRounds = 1;
    uint32_t connectRounds = 1;
    uint32_t settleMs = 100;
    uint32_t caseCooldownMs = 200;
    size_t threadCount = 0;
    uint32_t retryFailures = 1;
};

struct BenchmarkCase {
    std::string name;
    std::string executable;
    std::vector<std::string> args;
    bool isConnect = false;
    bool tls = false;
    std::string workloadModel;
};

struct CaseResult {
    BenchmarkCase benchmarkCase;
    int exitCode = -1;
    std::filesystem::path logPath;
    std::map<std::string, std::string> fields;
    uint32_t attempts = 1;
    bool recoveredAfterRetry = false;
};

std::string trim(std::string value) {
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return {};
    }
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

std::string normalizeLabel(std::string value) {
    value = trim(std::move(value));
    std::string normalized;
    normalized.reserve(value.size());
    bool lastWasSpace = false;
    for (unsigned char ch : value) {
        if (ch == ' ' || ch == '\t') {
            if (!lastWasSpace) {
                normalized.push_back(' ');
                lastWasSpace = true;
            }
            continue;
        }
        normalized.push_back(static_cast<char>(ch));
        lastWasSpace = false;
    }
    return normalized;
}

std::string quoteArg(const std::string& value) {
    std::string quoted = "\"";
    for (char ch : value) {
        if (ch == '"') {
            quoted += "\\\"";
        } else {
            quoted.push_back(ch);
        }
    }
    quoted.push_back('"');
    return quoted;
}

#ifdef _WIN32
std::string windowsCmdQuote(const std::string& value) {
    std::string quoted = "\"";
    quoted += value;
    quoted.push_back('"');
    return quoted;
}
#endif

std::string shellToken(const std::string& value) {
    if (value.find_first_of(" \t\"") == std::string::npos) {
        return value;
    }
#ifdef _WIN32
    return windowsCmdQuote(value);
#else
    return quoteArg(value);
#endif
}

std::string executableName(const std::string& baseName) {
#ifdef _WIN32
    return baseName + ".exe";
#else
    return baseName;
#endif
}

std::string makeCaseSlug(const std::string& value) {
    std::string slug;
    slug.reserve(value.size());
    for (unsigned char ch : value) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')) {
            slug.push_back(static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch));
        } else if (!slug.empty() && slug.back() != '-') {
            slug.push_back('-');
        }
    }
    while (!slug.empty() && slug.back() == '-') {
        slug.pop_back();
    }
    return slug.empty() ? "benchmark" : slug;
}

std::string formatDuration(std::chrono::seconds seconds) {
    const auto totalSeconds = seconds.count();
    const auto hours = totalSeconds / 3600;
    const auto minutes = (totalSeconds % 3600) / 60;
    const auto secs = totalSeconds % 60;
    std::ostringstream output;
    if (hours != 0) {
        output << hours << "h ";
        output << std::setw(2) << std::setfill('0') << minutes << "m ";
        output << std::setw(2) << std::setfill('0') << secs << "s";
        return output.str();
    }
    if (minutes != 0) {
        output << minutes << "m ";
        output << std::setw(2) << std::setfill('0') << secs << "s";
        return output.str();
    }
    output << secs << "s";
    return output.str();
}

std::chrono::seconds estimateLoopbackCaseFloor(const RunnerOptions& options) {
    return std::chrono::seconds(options.loopbackWarmupSeconds + options.loopbackDurationSeconds) +
           std::chrono::duration_cast<std::chrono::seconds>(std::chrono::milliseconds(options.settleMs + options.caseCooldownMs));
}

std::chrono::seconds estimateConnectCaseFloor(const RunnerOptions& options) {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::milliseconds(options.caseCooldownMs));
}

std::chrono::seconds estimateRemainingFloor(const RunnerOptions& options,
                                            const std::vector<BenchmarkCase>& cases,
                                            size_t nextIndex) {
    std::chrono::seconds total{0};
    for (size_t index = nextIndex; index < cases.size(); ++index) {
        total += cases[index].isConnect ? estimateConnectCaseFloor(options) : estimateLoopbackCaseFloor(options);
    }
    return total;
}

void applyProfileDefaults(RunnerOptions& options) {
    switch (options.profile) {
    case RunnerOptions::Profile::Smoke:
        options.loopbackClients = 4;
        options.loopbackMaxInflight = 4;
        options.loopbackTargetQps = 5000;
        options.connectClients = 64;
        options.payloadBytes = 256;
        options.loopbackWarmupSeconds = 1;
        options.loopbackDurationSeconds = 1;
        options.loopbackRounds = 1;
        options.connectWarmupRounds = 1;
        options.connectRounds = 1;
        options.settleMs = 100;
        options.caseCooldownMs = 200;
        break;
    case RunnerOptions::Profile::Standard:
        options.loopbackClients = 16;
        options.loopbackMaxInflight = 8;
        options.loopbackTargetQps = 20000;
        options.connectClients = 128;
        options.payloadBytes = 1024;
        options.loopbackWarmupSeconds = 2;
        options.loopbackDurationSeconds = 5;
        options.loopbackRounds = 3;
        options.connectWarmupRounds = 1;
        options.connectRounds = 3;
        options.settleMs = 200;
        options.caseCooldownMs = 500;
        break;
    case RunnerOptions::Profile::High:
        options.loopbackClients = 32;
        options.loopbackMaxInflight = 8;
        options.loopbackTargetQps = 40000;
        options.connectClients = 256;
        options.payloadBytes = 1024;
        options.loopbackWarmupSeconds = 2;
        options.loopbackDurationSeconds = 8;
        options.loopbackRounds = 1;
        options.connectWarmupRounds = 1;
        options.connectRounds = 3;
        options.settleMs = 300;
        options.caseCooldownMs = 1500;
        break;
    case RunnerOptions::Profile::Soak:
        options.loopbackClients = 32;
        options.loopbackMaxInflight = 8;
        options.loopbackTargetQps = 40000;
        options.connectClients = 256;
        options.payloadBytes = 1024;
        options.loopbackWarmupSeconds = 5;
        options.loopbackDurationSeconds = 60;
        options.loopbackRounds = 1;
        options.connectWarmupRounds = 2;
        options.connectRounds = 5;
        options.settleMs = 500;
        options.caseCooldownMs = 3000;
        break;
    }
}

bool parseUnsigned(const char* text, unsigned long long& value) {
    if (text == nullptr || *text == '\0') {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const unsigned long long parsed = std::strtoull(text, &end, 10);
    if (errno != 0 || end == text || (end != nullptr && *end != '\0')) {
        return false;
    }
    value = parsed;
    return true;
}

bool parseSizeValue(const char* text, size_t& value) {
    unsigned long long parsed = 0;
    if (!parseUnsigned(text, parsed)) {
        return false;
    }
    value = static_cast<size_t>(parsed);
    return true;
}

bool parseUint32Value(const char* text, uint32_t& value) {
    unsigned long long parsed = 0;
    if (!parseUnsigned(text, parsed)) {
        return false;
    }
    value = static_cast<uint32_t>(parsed);
    return true;
}

void printUsage() {
    std::cout
        << "Usage: fastnet_benchmark_matrix_runner [options]\n"
        << "  --profile <smoke|standard|high|soak>   Preset benchmark profile\n"
        << "  --bin-dir <path>                       Benchmark executable directory\n"
        << "  --output <path>                        Markdown report path\n"
        << "  --ssl                                  Include TLS variants\n"
        << "  --loopback-clients <value>             Override loopback client count\n"
        << "  --loopback-max-inflight <value>        Override loopback per-connection window\n"
        << "  --loopback-target-qps <value>          Add target-QPS loopback cases (0 disables)\n"
        << "  --connect-clients <value>              Override connect-burst client count\n"
        << "  --payload <bytes>                      Override loopback payload size\n"
        << "  --loopback-warmup <seconds>            Override loopback warmup\n"
        << "  --loopback-duration <seconds>          Override loopback duration\n"
        << "  --loopback-rounds <count>              Override loopback rounds\n"
        << "  --connect-warmup-rounds <count>        Override connect warmup rounds\n"
        << "  --connect-rounds <count>               Override connect rounds\n"
        << "  --settle-ms <ms>                       Override settle delay\n"
        << "  --case-cooldown-ms <ms>                Idle gap between benchmark processes\n"
        << "  --threads <value>                      Override FastNet IO threads\n"
        << "  --retry-failures <count>               Retry failed cases (default: 1)\n"
        << "  --help                                 Show this help\n";
}

bool parseProfile(std::string value, RunnerOptions::Profile& profile) {
    value = trim(std::move(value));
    if (value == "smoke") {
        profile = RunnerOptions::Profile::Smoke;
        return true;
    }
    if (value == "standard") {
        profile = RunnerOptions::Profile::Standard;
        return true;
    }
    if (value == "high") {
        profile = RunnerOptions::Profile::High;
        return true;
    }
    if (value == "soak") {
        profile = RunnerOptions::Profile::Soak;
        return true;
    }
    return false;
}

void scanProfileArg(int argc, char** argv, RunnerOptions::Profile& profile) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--profile") == 0) {
            RunnerOptions::Profile parsedProfile = profile;
            if (parseProfile(argv[i + 1], parsedProfile)) {
                profile = parsedProfile;
            }
            return;
        }
    }
}

bool parseArgs(int argc, char** argv, RunnerOptions& options) {
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (std::strcmp(arg, "--help") == 0) {
            printUsage();
            return false;
        }
        if (std::strcmp(arg, "--ssl") == 0) {
            options.includeTls = true;
            continue;
        }
        if (i + 1 >= argc) {
            std::cerr << "Missing value for argument: " << arg << '\n';
            return false;
        }

        const char* value = argv[++i];
        if (std::strcmp(arg, "--profile") == 0) {
            if (!parseProfile(value, options.profile)) {
                std::cerr << "Unknown profile: " << value << '\n';
                return false;
            }
        } else if (std::strcmp(arg, "--bin-dir") == 0) {
            options.binDir = value;
        } else if (std::strcmp(arg, "--output") == 0) {
            options.outputPath = value;
        } else if (std::strcmp(arg, "--loopback-clients") == 0) {
            if (!parseSizeValue(value, options.loopbackClients) || options.loopbackClients == 0) {
                return false;
            }
        } else if (std::strcmp(arg, "--loopback-max-inflight") == 0) {
            if (!parseSizeValue(value, options.loopbackMaxInflight) || options.loopbackMaxInflight == 0) {
                return false;
            }
        } else if (std::strcmp(arg, "--loopback-target-qps") == 0) {
            if (!parseSizeValue(value, options.loopbackTargetQps)) {
                return false;
            }
        } else if (std::strcmp(arg, "--connect-clients") == 0) {
            if (!parseSizeValue(value, options.connectClients) || options.connectClients == 0) {
                return false;
            }
        } else if (std::strcmp(arg, "--payload") == 0) {
            if (!parseSizeValue(value, options.payloadBytes) || options.payloadBytes == 0) {
                return false;
            }
        } else if (std::strcmp(arg, "--loopback-warmup") == 0) {
            if (!parseUint32Value(value, options.loopbackWarmupSeconds)) {
                return false;
            }
        } else if (std::strcmp(arg, "--loopback-duration") == 0) {
            if (!parseUint32Value(value, options.loopbackDurationSeconds) || options.loopbackDurationSeconds == 0) {
                return false;
            }
        } else if (std::strcmp(arg, "--loopback-rounds") == 0) {
            if (!parseUint32Value(value, options.loopbackRounds) || options.loopbackRounds == 0) {
                return false;
            }
        } else if (std::strcmp(arg, "--connect-warmup-rounds") == 0) {
            if (!parseUint32Value(value, options.connectWarmupRounds)) {
                return false;
            }
        } else if (std::strcmp(arg, "--connect-rounds") == 0) {
            if (!parseUint32Value(value, options.connectRounds) || options.connectRounds == 0) {
                return false;
            }
        } else if (std::strcmp(arg, "--settle-ms") == 0) {
            if (!parseUint32Value(value, options.settleMs)) {
                return false;
            }
        } else if (std::strcmp(arg, "--case-cooldown-ms") == 0) {
            if (!parseUint32Value(value, options.caseCooldownMs)) {
                return false;
            }
        } else if (std::strcmp(arg, "--threads") == 0) {
            if (!parseSizeValue(value, options.threadCount)) {
                return false;
            }
        } else if (std::strcmp(arg, "--retry-failures") == 0) {
            if (!parseUint32Value(value, options.retryFailures)) {
                return false;
            }
        } else {
            std::cerr << "Unknown argument: " << arg << '\n';
            return false;
        }
    }
    return true;
}

std::vector<BenchmarkCase> buildCases(const RunnerOptions& options) {
    std::vector<BenchmarkCase> cases;
    const std::vector<bool> tlsModes = options.includeTls ? std::vector<bool>{false, true} : std::vector<bool>{false};
    uint16_t nextPort = 9500;

    auto withPort = [&nextPort](std::vector<std::string> args) {
        args.push_back("--port");
        args.push_back(std::to_string(nextPort++));
        return args;
    };

    for (bool tls : tlsModes) {
        const std::string tlsSuffix = tls ? " TLS" : "";
        const std::vector<std::string> loopbackArgs = {
            "--clients",
            std::to_string(options.loopbackClients),
            "--max-inflight",
            std::to_string(options.loopbackMaxInflight),
            "--payload",
            std::to_string(options.payloadBytes),
            "--warmup",
            std::to_string(options.loopbackWarmupSeconds),
            "--duration",
            std::to_string(options.loopbackDurationSeconds),
            "--rounds",
            std::to_string(options.loopbackRounds),
            "--settle-ms",
            std::to_string(options.settleMs),
            "--threads",
            std::to_string(options.threadCount)};
        const std::vector<std::string> connectArgs = {
            "--clients",
            std::to_string(options.connectClients),
            "--warmup-rounds",
            std::to_string(options.connectWarmupRounds),
            "--rounds",
            std::to_string(options.connectRounds),
            "--settle-ms",
            std::to_string(options.settleMs),
            "--threads",
            std::to_string(options.threadCount)};

        auto appendTls = [tls](std::vector<std::string> args) {
            if (tls) {
                args.push_back("--ssl");
            }
            return args;
        };

        auto pushLoopbackCase = [&](const std::string& protocolName,
                                    const std::string& executableBaseName,
                                    const std::vector<std::string>& extraArgs,
                                    const std::string& modelSuffix,
                                    const std::string& workloadModel) {
            std::vector<std::string> args = loopbackArgs;
            args.insert(args.end(), extraArgs.begin(), extraArgs.end());
            cases.push_back({protocolName + " Loopback" + modelSuffix + tlsSuffix,
                             executableName(executableBaseName),
                             appendTls(withPort(std::move(args))),
                             false,
                             tls,
                             workloadModel});
        };

        pushLoopbackCase("TCP", "fastnet_tcp_loopback_benchmark", {}, "", "closed-loop");
        pushLoopbackCase("HTTP", "fastnet_http_loopback_benchmark", {}, "", "closed-loop");
        pushLoopbackCase("WebSocket", "fastnet_websocket_loopback_benchmark", {}, "", "closed-loop");
        if (options.loopbackTargetQps != 0 && !tls) {
            const std::vector<std::string> qpsArgs = {
                "--target-qps",
                std::to_string(options.loopbackTargetQps)};
            pushLoopbackCase("TCP", "fastnet_tcp_loopback_benchmark", qpsArgs, " QPS", "target-qps");
            pushLoopbackCase("HTTP", "fastnet_http_loopback_benchmark", qpsArgs, " QPS", "target-qps");
            pushLoopbackCase("WebSocket", "fastnet_websocket_loopback_benchmark", qpsArgs, " QPS", "target-qps");
        }
        cases.push_back({"TCP Connect Burst" + tlsSuffix,
                         executableName("fastnet_tcp_connect_burst_benchmark"),
                         appendTls(withPort(connectArgs)),
                         true,
                         tls,
                         "connect-burst"});
        cases.push_back({"HTTP Connect Burst" + tlsSuffix,
                         executableName("fastnet_http_connect_burst_benchmark"),
                         appendTls(withPort(connectArgs)),
                         true,
                         tls,
                         "connect-burst"});
        cases.push_back({"WebSocket Connect Burst" + tlsSuffix,
                         executableName("fastnet_websocket_connect_burst_benchmark"),
                         appendTls(withPort(connectArgs)),
                         true,
                         tls,
                         "connect-burst"});
    }

    return cases;
}

std::string buildCommand(const std::filesystem::path& executable,
                         const std::vector<std::string>& args,
                         const std::filesystem::path& logPath) {
    std::ostringstream command;
#ifdef _WIN32
    std::ostringstream inner;
    inner << windowsCmdQuote(executable.string());
    for (const auto& arg : args) {
        inner << ' ' << shellToken(arg);
    }
    inner << " > " << windowsCmdQuote(logPath.string()) << " 2>&1";
    command << "cmd /S /C " << windowsCmdQuote(inner.str());
#else
    command << quoteArg(executable.string());
    for (const auto& arg : args) {
        command << ' ' << quoteArg(arg);
    }
    command << " > " << quoteArg(logPath.string()) << " 2>&1";
#endif
    return command.str();
}

std::map<std::string, std::string> parseSummaryFields(const std::filesystem::path& logPath) {
    std::ifstream input(logPath);
    std::map<std::string, std::string> fields;
    std::string line;
    while (std::getline(input, line)) {
        const auto colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        const std::string label = normalizeLabel(line.substr(0, colon));
        const std::string value = trim(line.substr(colon + 1));
        if (!label.empty() && !value.empty()) {
            fields[label] = value;
        }
    }
    return fields;
}

bool hasExpectedSummaryFields(const CaseResult& result) {
    if (result.benchmarkCase.isConnect) {
        return result.fields.find("mean rate") != result.fields.end() &&
               result.fields.find("mean connect") != result.fields.end();
    }
    return result.fields.find("mean offered") != result.fields.end() &&
           result.fields.find("mean thrpt") != result.fields.end() &&
           result.fields.find("mean complete") != result.fields.end() &&
           result.fields.find("mean avg rtt") != result.fields.end();
}

CaseResult runCase(const BenchmarkCase& benchmarkCase,
                   const std::filesystem::path& binDir,
                   const std::filesystem::path& tempDir) {
    CaseResult result;
    result.benchmarkCase = benchmarkCase;
    const auto executable = binDir / benchmarkCase.executable;
    const auto logPath = tempDir / (makeCaseSlug(benchmarkCase.name) + std::string(".log"));
    result.logPath = logPath;

    std::error_code ec;
    std::filesystem::remove(logPath, ec);
    const std::string command = buildCommand(executable, benchmarkCase.args, logPath);
    result.exitCode = std::system(command.c_str());
    if (!std::filesystem::exists(logPath)) {
        std::ofstream logStream(logPath, std::ios::binary | std::ios::trunc);
        if (result.exitCode == -1) {
            logStream << "failed to start benchmark process\n";
        }
    }

    result.fields = parseSummaryFields(logPath);
    if (result.exitCode == 0 && !hasExpectedSummaryFields(result)) {
        result.exitCode = 2;
        std::ofstream logStream(logPath, std::ios::binary | std::ios::app);
        logStream << "\n[benchmark-matrix-runner] missing summary fields in benchmark output\n";
    }
    return result;
}

CaseResult runCaseWithRetries(const BenchmarkCase& benchmarkCase,
                              const std::filesystem::path& binDir,
                              const std::filesystem::path& tempDir,
                              uint32_t retryFailures,
                              uint32_t retryDelayMs) {
    CaseResult result = runCase(benchmarkCase, binDir, tempDir);
    for (uint32_t attempt = 1; result.exitCode != 0 && attempt <= retryFailures; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(retryDelayMs));
        CaseResult retryResult = runCase(benchmarkCase, binDir, tempDir);
        retryResult.attempts = attempt + 1;
        retryResult.recoveredAfterRetry = retryResult.exitCode == 0;
        result = std::move(retryResult);
    }
    return result;
}

std::string fieldOrDefault(const CaseResult& result, const std::string& key) {
    const auto it = result.fields.find(key);
    return it != result.fields.end() ? it->second : "n/a";
}

std::string buildMarkdownReport(const RunnerOptions& options,
                                const std::vector<CaseResult>& results,
                                const std::filesystem::path& reportPath) {
    std::ostringstream report;
    report << "# FastNet Benchmark Matrix Report\n\n";
    report << "- Profile: ";
    switch (options.profile) {
    case RunnerOptions::Profile::Smoke:
        report << "smoke";
        break;
    case RunnerOptions::Profile::Standard:
        report << "standard";
        break;
    case RunnerOptions::Profile::High:
        report << "high";
        break;
    case RunnerOptions::Profile::Soak:
        report << "soak";
        break;
    }
    report << '\n';
    report << "- TLS variants: " << (options.includeTls ? "enabled" : "disabled") << '\n';
    report << "- Loopback clients: " << options.loopbackClients << '\n';
    report << "- Loopback max inflight: " << options.loopbackMaxInflight << '\n';
    report << "- Loopback target QPS: "
           << (options.loopbackTargetQps == 0 ? std::string("disabled") : std::to_string(options.loopbackTargetQps))
           << '\n';
    report << "- TLS target-QPS loopback: disabled in matrix runner\n";
    report << "- Connect clients: " << options.connectClients << '\n';
    report << "- Payload bytes: " << options.payloadBytes << '\n';
    report << "- Report path: " << reportPath.string() << "\n\n";

    report << "## Loopback Summary\n\n";
    report << "| Case | Model | Status | Mean Offered | Mean Throughput | Mean Completion | Mean Avg RTT | Mean P95 RTT | Mean P99 RTT | Client Errors |\n";
    report << "| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |\n";
    for (const auto& result : results) {
        if (result.benchmarkCase.isConnect) {
            continue;
        }
        report << "| " << result.benchmarkCase.name << " | "
               << result.benchmarkCase.workloadModel << " | "
               << (result.exitCode == 0 ? (result.recoveredAfterRetry ? "PASS (retry)" : "PASS") : "FAIL") << " | "
               << fieldOrDefault(result, "mean offered") << " | "
               << fieldOrDefault(result, "mean thrpt") << " | "
               << fieldOrDefault(result, "mean complete") << " | "
               << fieldOrDefault(result, "mean avg rtt") << " | "
               << fieldOrDefault(result, "mean p95 rtt") << " | "
               << fieldOrDefault(result, "mean p99 rtt") << " | "
               << fieldOrDefault(result, "total cli errs") << " |\n";
    }

    report << "\n## Connect Burst Summary\n\n";
    report << "| Case | Status | Mean Rate | Mean Connect | Total Failures | Client Errors |\n";
    report << "| --- | --- | --- | --- | --- | --- |\n";
    for (const auto& result : results) {
        if (!result.benchmarkCase.isConnect) {
            continue;
        }
        report << "| " << result.benchmarkCase.name << " | "
               << (result.exitCode == 0 ? (result.recoveredAfterRetry ? "PASS (retry)" : "PASS") : "FAIL") << " | "
               << fieldOrDefault(result, "mean rate") << " | "
               << fieldOrDefault(result, "mean connect") << " | "
               << fieldOrDefault(result, "total failures") << " | "
               << fieldOrDefault(result, "total cli errs") << " |\n";
    }

    report << "\n## Logs\n\n";
    for (const auto& result : results) {
        report << "- " << result.benchmarkCase.name << ": " << result.logPath.string()
               << " (attempts=" << result.attempts << ")\n";
    }

    return report.str();
}

} // namespace

int main(int argc, char** argv) {
    RunnerOptions options;
    scanProfileArg(argc, argv, options.profile);
    applyProfileDefaults(options);
    if (!parseArgs(argc, argv, options)) {
        return 1;
    }

    const std::filesystem::path executablePath = std::filesystem::absolute(argv[0]);
    if (options.binDir.empty()) {
        options.binDir = executablePath.parent_path();
    }

    std::error_code ec;
    const auto tmpRoot = std::filesystem::current_path() / "tmp";
    std::filesystem::create_directories(tmpRoot, ec);
    const auto runDir = tmpRoot / ("benchmark-matrix-" + std::to_string(
        static_cast<long long>(std::chrono::steady_clock::now().time_since_epoch().count())));
    std::filesystem::create_directories(runDir, ec);

    if (options.outputPath.empty()) {
        options.outputPath = runDir / "benchmark-matrix-report.md";
    }

    std::cout << "FastNet benchmark matrix runner\n"
              << "  bin dir  : " << options.binDir.string() << '\n'
              << "  output   : " << options.outputPath.string() << '\n'
              << "  tls      : " << (options.includeTls ? "ON" : "OFF") << '\n'
              << "  loopback : clients=" << options.loopbackClients
              << " inflight=" << options.loopbackMaxInflight
              << " payload=" << options.payloadBytes
              << " duration=" << options.loopbackDurationSeconds << "s"
              << " target-qps="
              << (options.loopbackTargetQps == 0 ? std::string("OFF") : std::to_string(options.loopbackTargetQps))
              << '\n'
              << "  connect  : clients=" << options.connectClients
              << " rounds=" << options.connectRounds << '\n'
              << "  cooldown : " << options.caseCooldownMs << " ms\n";

    const auto cases = buildCases(options);
    const auto totalFloor = estimateRemainingFloor(options, cases, 0);
    size_t loopbackCaseCount = 0;
    size_t connectCaseCount = 0;
    for (const auto& benchmarkCase : cases) {
        if (benchmarkCase.isConnect) {
            ++connectCaseCount;
        } else {
            ++loopbackCaseCount;
        }
    }
    std::cout << "  cases    : " << cases.size()
              << " (loopback=" << loopbackCaseCount
              << ", connect=" << connectCaseCount << ")\n"
              << "  floor ETA: " << formatDuration(totalFloor)
              << " + connect-case runtime\n";

    std::vector<CaseResult> results;
    results.reserve(cases.size());

    int exitCode = 0;
    const auto runnerStart = std::chrono::steady_clock::now();
    for (size_t caseIndex = 0; caseIndex < cases.size(); ++caseIndex) {
        const auto& benchmarkCase = cases[caseIndex];
        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - runnerStart);
        const auto remainingFloor = estimateRemainingFloor(options, cases, caseIndex);
        std::cout << "\n[run " << (caseIndex + 1) << '/' << cases.size() << "] " << benchmarkCase.name << '\n'
                  << "  elapsed   : " << formatDuration(elapsed) << '\n'
                  << "  floor left: " << formatDuration(remainingFloor) << " + connect-case runtime\n";
        const CaseResult result = runCaseWithRetries(
            benchmarkCase,
            options.binDir,
            runDir,
            options.retryFailures,
            (std::max)(options.caseCooldownMs, 200u));
        if (result.exitCode != 0) {
            exitCode = 1;
        }
        std::cout << "  status    : "
                  << (result.exitCode == 0 ? (result.recoveredAfterRetry ? "PASS (retry)" : "PASS") : "FAIL")
                  << '\n';
        if (result.attempts > 1) {
            std::cout << "  attempts  : " << result.attempts << '\n';
        }
        if (benchmarkCase.isConnect) {
            std::cout << "  mean rate : " << fieldOrDefault(result, "mean rate") << '\n'
                      << "  mean conn : " << fieldOrDefault(result, "mean connect") << '\n';
        } else {
            std::cout << "  model     : " << result.benchmarkCase.workloadModel << '\n'
                      << "  mean offer: " << fieldOrDefault(result, "mean offered") << '\n'
                      << "  mean thrpt: " << fieldOrDefault(result, "mean thrpt") << '\n'
                      << "  mean done : " << fieldOrDefault(result, "mean complete") << '\n'
                      << "  mean p95  : " << fieldOrDefault(result, "mean p95 rtt") << '\n'
                      << "  mean p99  : " << fieldOrDefault(result, "mean p99 rtt") << '\n';
        }
        results.push_back(result);
        if (options.caseCooldownMs != 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(options.caseCooldownMs));
        }
    }

    const std::string report = buildMarkdownReport(options, results, options.outputPath);
    std::ofstream output(options.outputPath, std::ios::binary | std::ios::trunc);
    output << report;
    output.close();

    const auto totalElapsed = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - runnerStart);
    std::cout << "\nTotal elapsed: " << formatDuration(totalElapsed) << '\n'
              << "Report written to: " << options.outputPath.string() << '\n';
    return exitCode;
}
