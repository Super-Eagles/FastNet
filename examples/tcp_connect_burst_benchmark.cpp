#include "BenchmarkCommon.h"
#include "BenchmarkTls.h"
#include "FastNet/FastNet.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

struct BenchmarkOptions {
    uint16_t port = 9330;
    size_t clients = 256;
    uint32_t warmupRounds = 1;
    uint32_t rounds = 3;
    uint32_t settleMs = 300;
    uint32_t connectTimeoutMs = 5000;
    size_t threadCount = 0;
    bool enableSsl = false;
};

struct ConnectRunResult {
    double seconds = 0.0;
    uint64_t connectedClients = 0;
    uint64_t serverReadySessions = 0;
    uint64_t failedClients = 0;
    uint64_t disconnectedClients = 0;
    uint64_t clientErrors = 0;
    uint64_t totalLatencyNs = 0;
    uint64_t maxLatencyNs = 0;

    bool fullSuccess(size_t expectedClients) const {
        return connectedClients == expectedClients && failedClients == 0;
    }

    double connectRatePerSecond() const {
        return seconds > 0.0 ? static_cast<double>(connectedClients) / seconds : 0.0;
    }

    double averageLatencyMs() const {
        return connectedClients != 0
                   ? static_cast<double>(totalLatencyNs) / static_cast<double>(connectedClients) / 1'000'000.0
                   : 0.0;
    }

    double maxLatencyMs() const {
        return static_cast<double>(maxLatencyNs) / 1'000'000.0;
    }
};

struct ConnectBenchmarkSummary {
    size_t rounds = 0;
    size_t successfulRounds = 0;
    double meanConnectRate = 0.0;
    double medianConnectRate = 0.0;
    double bestConnectRate = 0.0;
    double worstConnectRate = 0.0;
    double meanLatencyMs = 0.0;
    double medianLatencyMs = 0.0;
    double bestLatencyMs = 0.0;
    double worstLatencyMs = 0.0;
    uint64_t totalFailures = 0;
    uint64_t totalClientErrors = 0;
};

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

bool parseOptionValue(const char* text, size_t& value) {
    unsigned long long parsed = 0;
    if (!parseUnsigned(text, parsed) ||
        parsed > static_cast<unsigned long long>((std::numeric_limits<size_t>::max)())) {
        return false;
    }
    value = static_cast<size_t>(parsed);
    return true;
}

bool parseOptionValue(const char* text, uint32_t& value) {
    unsigned long long parsed = 0;
    if (!parseUnsigned(text, parsed) ||
        parsed > static_cast<unsigned long long>((std::numeric_limits<uint32_t>::max)())) {
        return false;
    }
    value = static_cast<uint32_t>(parsed);
    return true;
}

bool parseOptionValue(const char* text, uint16_t& value) {
    unsigned long long parsed = 0;
    if (!parseUnsigned(text, parsed) ||
        parsed > static_cast<unsigned long long>((std::numeric_limits<uint16_t>::max)())) {
        return false;
    }
    value = static_cast<uint16_t>(parsed);
    return true;
}

void printUsage() {
    std::cout
        << "Usage: fastnet_tcp_connect_burst_benchmark [options]\n"
        << "  --port <value>            Listen port (default: 9330)\n"
        << "  --clients <value>         Concurrent clients per round (default: 256)\n"
        << "  --warmup-rounds <count>   Unmeasured warmup rounds (default: 1)\n"
        << "  --rounds <count>          Measured rounds (default: 3)\n"
        << "  --settle-ms <ms>          Idle gap between rounds (default: 300)\n"
        << "  --connect-timeout <ms>    Client connect timeout (default: 5000)\n"
        << "  --threads <value>         FastNet IO threads, 0 = auto (default: 0)\n"
        << "  --ssl                     Enable loopback TLS (requires FASTNET_ENABLE_SSL build)\n"
        << "  --help                    Show this help\n";
}

bool parseArgs(int argc, char** argv, BenchmarkOptions& options) {
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (std::strcmp(arg, "--help") == 0) {
            printUsage();
            return false;
        }
        if (std::strcmp(arg, "--ssl") == 0) {
            options.enableSsl = true;
            continue;
        }
        if (i + 1 >= argc) {
            std::cerr << "Missing value for argument: " << arg << '\n';
            return false;
        }

        const char* value = argv[++i];
        if (std::strcmp(arg, "--port") == 0) {
            if (!parseOptionValue(value, options.port)) {
                return false;
            }
        } else if (std::strcmp(arg, "--clients") == 0) {
            if (!parseOptionValue(value, options.clients) || options.clients == 0) {
                return false;
            }
        } else if (std::strcmp(arg, "--warmup-rounds") == 0) {
            if (!parseOptionValue(value, options.warmupRounds)) {
                return false;
            }
        } else if (std::strcmp(arg, "--rounds") == 0) {
            if (!parseOptionValue(value, options.rounds) || options.rounds == 0) {
                return false;
            }
        } else if (std::strcmp(arg, "--settle-ms") == 0) {
            if (!parseOptionValue(value, options.settleMs)) {
                return false;
            }
        } else if (std::strcmp(arg, "--connect-timeout") == 0) {
            if (!parseOptionValue(value, options.connectTimeoutMs) || options.connectTimeoutMs == 0) {
                return false;
            }
        } else if (std::strcmp(arg, "--threads") == 0) {
            if (!parseOptionValue(value, options.threadCount)) {
                return false;
            }
        } else {
            std::cerr << "Unknown argument: " << arg << '\n';
            return false;
        }
    }
    return true;
}

struct SharedConnectState {
    std::atomic<uint64_t> connectedClients{0};
    std::atomic<uint64_t> serverReadySessions{0};
    std::atomic<uint64_t> failedClients{0};
    std::atomic<uint64_t> disconnectedClients{0};
    std::atomic<uint64_t> clientErrors{0};
    std::atomic<uint64_t> totalLatencyNs{0};
    std::atomic<uint64_t> maxLatencyNs{0};
    std::atomic<bool> shutdownPhase{false};
    std::mutex mutex;
    std::condition_variable condition;

    void recordLatency(uint64_t latencyNs) {
        totalLatencyNs.fetch_add(latencyNs, std::memory_order_relaxed);
        uint64_t observed = maxLatencyNs.load(std::memory_order_relaxed);
        while (observed < latencyNs &&
               !maxLatencyNs.compare_exchange_weak(observed, latencyNs, std::memory_order_relaxed)) {
        }
    }

    void notifyProgress() {
        std::lock_guard<std::mutex> lock(mutex);
        condition.notify_all();
    }
};

class ConnectClient : public std::enable_shared_from_this<ConnectClient> {
public:
    ConnectClient(FastNet::IoService& ioService,
                  FastNet::Address serverAddress,
                  std::shared_ptr<SharedConnectState> sharedState,
                  uint32_t connectTimeoutMs,
                  FastNet::SSLConfig sslConfig)
        : client_(ioService),
          serverAddress_(std::move(serverAddress)),
          sharedState_(std::move(sharedState)),
          connectTimeoutMs_(connectTimeoutMs),
          sslConfig_(std::move(sslConfig)) {}

    void startConnect() {
        startedAt_ = std::chrono::steady_clock::now();
        std::weak_ptr<ConnectClient> weakSelf = shared_from_this();
        client_.setDisconnectCallback([weakSelf](const std::string&) {
            auto self = weakSelf.lock();
            if (!self) {
                return;
            }
            self->handleDisconnect();
        });
        client_.setErrorCallback([weakSelf](FastNet::ErrorCode, const std::string&) {
            auto self = weakSelf.lock();
            if (!self) {
                return;
            }
            self->sharedState_->clientErrors.fetch_add(1, std::memory_order_relaxed);
            self->sharedState_->notifyProgress();
        });
        client_.setConnectTimeout(connectTimeoutMs_);
        client_.connect(
            serverAddress_,
            [weakSelf](bool success, const std::string&) {
                auto self = weakSelf.lock();
                if (!self) {
                    return;
                }
                self->handleConnect(success);
            },
            sslConfig_);
    }

    void disconnect() {
        if (connected_.load(std::memory_order_acquire)) {
            client_.disconnectAfterPendingWrites();
        } else {
            client_.disconnect();
        }
    }

private:
    void handleConnect(bool success) {
        if (success) {
            connected_.store(true, std::memory_order_release);
            const auto now = std::chrono::steady_clock::now();
            const auto latencyNs =
                static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now - startedAt_).count());
            sharedState_->recordLatency(latencyNs);
            sharedState_->connectedClients.fetch_add(1, std::memory_order_relaxed);
        } else {
            sharedState_->failedClients.fetch_add(1, std::memory_order_relaxed);
        }
        sharedState_->notifyProgress();
    }

    void handleDisconnect() {
        if (!connected_.exchange(false, std::memory_order_acq_rel)) {
            return;
        }
        sharedState_->disconnectedClients.fetch_add(1, std::memory_order_relaxed);
        sharedState_->notifyProgress();
    }

    FastNet::TcpClient client_;
    FastNet::Address serverAddress_;
    std::shared_ptr<SharedConnectState> sharedState_;
    uint32_t connectTimeoutMs_ = 0;
    FastNet::SSLConfig sslConfig_;
    std::chrono::steady_clock::time_point startedAt_{};
    std::atomic<bool> connected_{false};
};

bool waitForRoundReady(const std::shared_ptr<SharedConnectState>& state,
                       const BenchmarkOptions& options) {
    const auto waitDeadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(static_cast<uint64_t>(options.connectTimeoutMs) * 3);
    std::unique_lock<std::mutex> lock(state->mutex);
    return state->condition.wait_until(lock, waitDeadline, [&]() {
        const uint64_t connected = state->connectedClients.load(std::memory_order_acquire);
        const uint64_t failed = state->failedClients.load(std::memory_order_acquire);
        return connected + failed >= options.clients;
    });
}

ConnectRunResult runRound(FastNet::IoService& ioService, const BenchmarkOptions& options) {
    auto sharedState = std::make_shared<SharedConnectState>();
    FastNet::TcpServer server(ioService);
    FastNet::SSLConfig serverSsl;
    FastNet::SSLConfig clientSsl;
#ifdef FASTNET_ENABLE_SSL
    std::unique_ptr<FastNetExamples::TempTlsFiles> tlsFiles;
    if (options.enableSsl) {
        auto generatedTlsFiles =
            std::make_unique<FastNetExamples::TempTlsFiles>(FastNetExamples::makeTempTlsFiles("tcp-connect-burst"));
        std::string certificateError;
        if (!FastNetExamples::writeSelfSignedCertificate(*generatedTlsFiles, certificateError)) {
            std::cerr << "Failed to create loopback TLS certificate: " << certificateError << '\n';
            return {};
        }

        serverSsl.enableSSL = true;
        serverSsl.verifyPeer = false;
        serverSsl.certificateFile = generatedTlsFiles->certificate.string();
        serverSsl.privateKeyFile = generatedTlsFiles->privateKey.string();

        clientSsl.enableSSL = true;
        clientSsl.verifyPeer = false;
        tlsFiles = std::move(generatedTlsFiles);
    }
#endif
    server.setClientConnectedCallback([sharedState](FastNet::ConnectionId, const FastNet::Address&) {
        sharedState->serverReadySessions.fetch_add(1, std::memory_order_relaxed);
        sharedState->notifyProgress();
    });
    server.setServerErrorCallback([sharedState](const FastNet::Error& error) {
        if (!sharedState->shutdownPhase.load(std::memory_order_acquire)) {
            std::cerr << "[connect-burst-server] " << error.toString() << '\n';
        }
    });

    const FastNet::Error startResult = server.start(options.port, "127.0.0.1", serverSsl);
    if (startResult.isFailure()) {
        std::cerr << "Failed to start server: " << startResult.toString() << '\n';
        return {};
    }

    std::vector<std::shared_ptr<ConnectClient>> clients;
    clients.reserve(options.clients);
    const FastNet::Address serverAddress = server.getListenAddress();
    for (size_t i = 0; i < options.clients; ++i) {
        clients.push_back(std::make_shared<ConnectClient>(
            ioService, serverAddress, sharedState, options.connectTimeoutMs, clientSsl));
    }

    const auto roundStart = std::chrono::steady_clock::now();
    for (auto& client : clients) {
        client->startConnect();
    }

    waitForRoundReady(sharedState, options);
    const auto roundEnd = std::chrono::steady_clock::now();

    ConnectRunResult result;
    result.seconds = std::chrono::duration_cast<std::chrono::duration<double>>(roundEnd - roundStart).count();
    result.connectedClients = sharedState->connectedClients.load(std::memory_order_acquire);
    result.serverReadySessions = sharedState->serverReadySessions.load(std::memory_order_acquire);
    result.failedClients = sharedState->failedClients.load(std::memory_order_acquire);
    result.clientErrors = sharedState->clientErrors.load(std::memory_order_acquire);
    result.totalLatencyNs = sharedState->totalLatencyNs.load(std::memory_order_acquire);
    result.maxLatencyNs = sharedState->maxLatencyNs.load(std::memory_order_acquire);

    sharedState->shutdownPhase.store(true, std::memory_order_release);
    if (options.enableSsl) {
        server.stop();
    } else {
        for (auto& client : clients) {
            client->disconnect();
        }
    }

    {
        std::unique_lock<std::mutex> lock(sharedState->mutex);
        const auto disconnectDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        sharedState->condition.wait_until(lock, disconnectDeadline, [&]() {
            return sharedState->disconnectedClients.load(std::memory_order_acquire) >= result.connectedClients;
        });
    }

    result.disconnectedClients = sharedState->disconnectedClients.load(std::memory_order_acquire);
    if (!options.enableSsl) {
        server.stop();
    }
    return result;
}

void printRunResult(const ConnectRunResult& result, size_t roundIndex, size_t expectedClients) {
    std::cout << "\nRound " << roundIndex << '\n'
              << "  connected   : " << result.connectedClients << "/" << expectedClients << '\n'
              << "  server ready: " << result.serverReadySessions << "/" << expectedClients << '\n'
              << "  failed      : " << result.failedClients << '\n'
              << "  conn rate   : " << FastNetExamples::formatOpsRate(result.connectRatePerSecond()) << '\n';

    if (result.connectedClients != 0) {
        std::cout << "  avg connect : "
                  << FastNet::BenchmarkUtils::formatLatency(result.averageLatencyMs()) << '\n'
                  << "  max connect : "
                  << FastNet::BenchmarkUtils::formatLatency(result.maxLatencyMs()) << '\n';
    } else {
        std::cout << "  avg connect : n/a\n"
                  << "  max connect : n/a\n";
    }

    std::cout << "  disconnects : " << result.disconnectedClients << '\n'
              << "  client errs : " << result.clientErrors << '\n';
}

ConnectBenchmarkSummary summarizeRuns(const std::vector<ConnectRunResult>& runs, size_t expectedClients) {
    ConnectBenchmarkSummary summary;
    summary.rounds = runs.size();
    if (runs.empty()) {
        return summary;
    }

    std::vector<double> rates;
    std::vector<double> latencies;
    rates.reserve(runs.size());
    latencies.reserve(runs.size());

    bool firstSuccess = true;
    for (const auto& run : runs) {
        summary.totalFailures += run.failedClients;
        summary.totalClientErrors += run.clientErrors;
        if (!run.fullSuccess(expectedClients)) {
            continue;
        }

        const double rate = run.connectRatePerSecond();
        const double latency = run.averageLatencyMs();
        rates.push_back(rate);
        latencies.push_back(latency);
        ++summary.successfulRounds;
        summary.meanConnectRate += rate;
        summary.meanLatencyMs += latency;

        if (firstSuccess) {
            summary.bestConnectRate = rate;
            summary.worstConnectRate = rate;
            summary.bestLatencyMs = latency;
            summary.worstLatencyMs = latency;
            firstSuccess = false;
        } else {
            summary.bestConnectRate = (std::max)(summary.bestConnectRate, rate);
            summary.worstConnectRate = (std::min)(summary.worstConnectRate, rate);
            summary.bestLatencyMs = (std::min)(summary.bestLatencyMs, latency);
            summary.worstLatencyMs = (std::max)(summary.worstLatencyMs, latency);
        }
    }

    if (summary.successfulRounds == 0) {
        return summary;
    }

    std::sort(rates.begin(), rates.end());
    std::sort(latencies.begin(), latencies.end());
    const size_t mid = rates.size() / 2;
    if ((rates.size() % 2) == 0) {
        summary.medianConnectRate = (rates[mid - 1] + rates[mid]) / 2.0;
        summary.medianLatencyMs = (latencies[mid - 1] + latencies[mid]) / 2.0;
    } else {
        summary.medianConnectRate = rates[mid];
        summary.medianLatencyMs = latencies[mid];
    }

    summary.meanConnectRate /= static_cast<double>(summary.successfulRounds);
    summary.meanLatencyMs /= static_cast<double>(summary.successfulRounds);
    return summary;
}

void printSummary(const ConnectBenchmarkSummary& summary) {
    std::cout << "\nSummary\n"
              << "  rounds         : " << summary.rounds << '\n'
              << "  successful     : " << summary.successfulRounds << '\n'
              << "  mean rate      : " << FastNetExamples::formatOpsRate(summary.meanConnectRate) << '\n'
              << "  median rate    : " << FastNetExamples::formatOpsRate(summary.medianConnectRate) << '\n'
              << "  best rate      : " << FastNetExamples::formatOpsRate(summary.bestConnectRate) << '\n'
              << "  worst rate     : " << FastNetExamples::formatOpsRate(summary.worstConnectRate) << '\n';

    if (summary.successfulRounds != 0) {
        std::cout << "  mean connect   : " << FastNet::BenchmarkUtils::formatLatency(summary.meanLatencyMs) << '\n'
                  << "  median connect : " << FastNet::BenchmarkUtils::formatLatency(summary.medianLatencyMs) << '\n'
                  << "  best connect   : " << FastNet::BenchmarkUtils::formatLatency(summary.bestLatencyMs) << '\n'
                  << "  worst connect  : " << FastNet::BenchmarkUtils::formatLatency(summary.worstLatencyMs) << '\n';
    } else {
        std::cout << "  mean connect   : n/a\n"
                  << "  median connect : n/a\n"
                  << "  best connect   : n/a\n"
                  << "  worst connect  : n/a\n";
    }

    std::cout << "  total failures : " << summary.totalFailures << '\n'
              << "  total cli errs : " << summary.totalClientErrors << '\n';
}

} // namespace

int main(int argc, char** argv) {
    BenchmarkOptions options;
    if (!parseArgs(argc, argv, options)) {
        return 1;
    }

#ifndef FASTNET_ENABLE_SSL
    if (options.enableSsl) {
        std::cerr << "This benchmark binary was built without FASTNET_ENABLE_SSL support.\n";
        return 1;
    }
#endif

    if (FastNet::initialize(options.threadCount) != FastNet::ErrorCode::Success) {
        std::cerr << "Failed to initialize FastNet\n";
        return 1;
    }

    auto& ioService = FastNet::getGlobalIoService();
    std::cout << "FastNet TCP connect burst benchmark\n"
              << "  mode      : " << (options.enableSsl ? "TLS" : "Plain") << '\n'
              << "  clients   : " << options.clients << '\n'
              << "  warmup    : " << options.warmupRounds << '\n'
              << "  rounds    : " << options.rounds << '\n'
              << "  settle    : " << options.settleMs << " ms\n"
              << "  timeout   : " << options.connectTimeoutMs << " ms\n"
              << "  io threads: " << ioService.getThreadCount() << "\n";

    std::vector<ConnectRunResult> runs;
    runs.reserve(options.rounds);
    int exitCode = 0;

    for (uint32_t warmup = 0; warmup < options.warmupRounds; ++warmup) {
        (void)runRound(ioService, options);
        if (warmup + 1 < options.warmupRounds && options.settleMs != 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(options.settleMs));
        }
    }

    for (uint32_t round = 0; round < options.rounds; ++round) {
        const ConnectRunResult result = runRound(ioService, options);
        printRunResult(result, static_cast<size_t>(round + 1), options.clients);
        runs.push_back(result);
        if (!result.fullSuccess(options.clients)) {
            exitCode = 1;
        }
        if (round + 1 < options.rounds && options.settleMs != 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(options.settleMs));
        }
    }

    printSummary(summarizeRuns(runs, options.clients));
    FastNet::cleanup();
    return exitCode;
}
