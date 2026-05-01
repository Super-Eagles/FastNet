#include "BenchmarkCommon.h"
#include "BenchmarkTls.h"
#include "FastNet/FastNet.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

struct BenchmarkOptions {
    uint16_t port = 9200;
    size_t clients = 32;
    size_t maxInflight = 1;
    size_t targetQps = 0;
    size_t payloadBytes = 1024;
    uint32_t warmupSeconds = 1;
    uint32_t durationSeconds = 5;
    uint32_t rounds = 3;
    uint32_t settleMs = 200;
    uint32_t connectTimeoutMs = 30000;
    size_t threadCount = 0;
    bool enableSsl = false;
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
        << "Usage: fastnet_websocket_loopback_benchmark [options]\n"
        << "  --port <value>            Listen port (default: 9200)\n"
        << "  --clients <value>         Concurrent clients (default: 32)\n"
        << "  --max-inflight <value>    Max in-flight messages per connection (default: 1)\n"
        << "  --target-qps <value>      Target offered QPS, 0 = closed-loop (default: 0)\n"
        << "  --payload <bytes>         Echo payload size (default: 1024)\n"
        << "  --warmup <seconds>        Warmup duration (default: 1)\n"
        << "  --duration <seconds>      Measured duration (default: 5)\n"
        << "  --rounds <count>          Measured rounds (default: 3)\n"
        << "  --settle-ms <ms>          Idle gap between rounds (default: 200)\n"
        << "  --connect-timeout <ms>    Client connect timeout (default: 30000)\n"
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
        } else if (std::strcmp(arg, "--max-inflight") == 0) {
            if (!parseOptionValue(value, options.maxInflight) || options.maxInflight == 0) {
                return false;
            }
        } else if (std::strcmp(arg, "--target-qps") == 0) {
            if (!parseOptionValue(value, options.targetQps)) {
                return false;
            }
        } else if (std::strcmp(arg, "--payload") == 0) {
            if (!parseOptionValue(value, options.payloadBytes) || options.payloadBytes == 0) {
                return false;
            }
        } else if (std::strcmp(arg, "--warmup") == 0) {
            if (!parseOptionValue(value, options.warmupSeconds)) {
                return false;
            }
        } else if (std::strcmp(arg, "--duration") == 0) {
            if (!parseOptionValue(value, options.durationSeconds) || options.durationSeconds == 0) {
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

struct SharedBenchmarkState {
    std::atomic<uint64_t> connectedClients{0};
    std::atomic<uint64_t> readyServerSessions{0};
    std::atomic<uint64_t> failedClients{0};
    std::atomic<uint64_t> disconnectedClients{0};
    std::atomic<uint64_t> sendAttempts{0};
    std::atomic<uint64_t> serverReceivedMessages{0};
    std::atomic<uint64_t> serverReceivedBytes{0};
    std::atomic<uint64_t> clientReceivedBytes{0};
    std::atomic<uint64_t> completedRoundTrips{0};
    std::atomic<uint64_t> totalLatencyNs{0};
    std::atomic<uint64_t> maxLatencyNs{0};
    std::atomic<uint64_t> clientErrors{0};
    FastNetExamples::AtomicLatencyHistogram latencyHistogram;
    std::atomic<bool> benchmarkRunning{false};
    std::atomic<bool> shutdownPhase{false};
    std::mutex mutex;
    std::condition_variable condition;
    std::string firstConnectFailure;

    void resetMetrics() {
        sendAttempts.store(0, std::memory_order_release);
        serverReceivedMessages.store(0, std::memory_order_release);
        serverReceivedBytes.store(0, std::memory_order_release);
        clientReceivedBytes.store(0, std::memory_order_release);
        completedRoundTrips.store(0, std::memory_order_release);
        totalLatencyNs.store(0, std::memory_order_release);
        maxLatencyNs.store(0, std::memory_order_release);
        latencyHistogram.reset();
        std::lock_guard<std::mutex> lock(mutex);
        firstConnectFailure.clear();
    }

    void recordLatency(uint64_t latencyNs) {
        totalLatencyNs.fetch_add(latencyNs, std::memory_order_relaxed);
        latencyHistogram.record(latencyNs);
        uint64_t observed = maxLatencyNs.load(std::memory_order_relaxed);
        while (observed < latencyNs &&
               !maxLatencyNs.compare_exchange_weak(observed, latencyNs, std::memory_order_relaxed)) {
        }
    }

    void notifyProgress() {
        std::lock_guard<std::mutex> lock(mutex);
        condition.notify_all();
    }

    void recordConnectFailure(const std::string& message) {
        std::lock_guard<std::mutex> lock(mutex);
        if (firstConnectFailure.empty()) {
            firstConnectFailure = message.empty() ? "unknown connect failure" : message;
        }
        condition.notify_all();
    }
};

class BenchmarkClient : public std::enable_shared_from_this<BenchmarkClient> {
public:
    BenchmarkClient(FastNet::IoService& ioService,
                    std::string url,
                    std::shared_ptr<SharedBenchmarkState> sharedState,
                    const BenchmarkOptions& options,
                    FastNet::SSLConfig sslConfig)
        : ioService_(ioService),
          client_(ioService),
          url_(std::move(url)),
          sharedState_(std::move(sharedState)),
          payload_(options.payloadBytes, static_cast<std::uint8_t>('x')),
          maxInflight_((std::max)(static_cast<size_t>(1), options.maxInflight)),
          closedLoopMode_(options.targetQps == 0),
          scheduleSendsOnIoThread_(options.enableSsl && options.targetQps != 0),
          connectTimeoutMs_(options.connectTimeoutMs),
          sslConfig_(std::move(sslConfig)) {}

    void startConnect() {
        std::weak_ptr<BenchmarkClient> weakSelf = shared_from_this();
        client_.setOwnedBinaryCallback([weakSelf](FastNet::Buffer&& data) {
            auto self = weakSelf.lock();
            if (!self) {
                return;
            }
            self->handleBinary(data);
        });
        client_.setCloseCallback([weakSelf](uint16_t, const std::string&) {
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
        });
        client_.setConnectTimeout(connectTimeoutMs_);
        client_.setSSLConfig(sslConfig_);
        client_.connect(url_, [weakSelf](bool success, const std::string& message) {
            auto self = weakSelf.lock();
            if (!self) {
                return;
            }
            self->handleConnect(success, message);
        });
    }

    void startTraffic() {
        if (closedLoopMode_) {
            fillWindow();
        }
    }

    void disconnect() {
        client_.close();
    }

    bool trySendOne() {
        const auto queuedAt = FastNet::BenchmarkUtils::now();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!connected_ || inFlight_ >= maxInflight_) {
                return false;
            }
            ++inFlight_;
        }

        if (scheduleSendsOnIoThread_) {
            std::weak_ptr<BenchmarkClient> weakSelf = shared_from_this();
            ioService_.post([weakSelf, queuedAt]() {
                auto self = weakSelf.lock();
                if (!self) {
                    return;
                }
                self->performQueuedSend(queuedAt);
            });
            return true;
        }
        return performQueuedSend(queuedAt);
    }

    bool isIdle() {
        std::lock_guard<std::mutex> lock(mutex_);
        return inFlight_ == 0;
    }

private:
    bool performQueuedSend(std::chrono::steady_clock::time_point) {
        const auto sentAt = FastNet::BenchmarkUtils::now();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!connected_ || inFlight_ == 0) {
                return false;
            }
            sendTimes_.push_back(sentAt);
        }

        sharedState_->sendAttempts.fetch_add(1, std::memory_order_relaxed);
        if (!client_.sendBinary(payload_)) {
            rollbackQueuedSend();
            return false;
        }
        return true;
    }

    void rollbackQueuedSend() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (inFlight_ != 0) {
                --inFlight_;
            }
            if (!sendTimes_.empty()) {
                sendTimes_.pop_back();
            }
        }
        sharedState_->clientErrors.fetch_add(1, std::memory_order_relaxed);
    }

    void handleConnect(bool success, const std::string& message) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            connected_ = success;
        }
        if (success) {
            sharedState_->connectedClients.fetch_add(1, std::memory_order_relaxed);
        } else {
            sharedState_->failedClients.fetch_add(1, std::memory_order_relaxed);
            sharedState_->recordConnectFailure(message);
        }
        sharedState_->notifyProgress();
    }

    void handleDisconnect() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            connected_ = false;
            inFlight_ = 0;
            sendTimes_.clear();
        }
        sharedState_->disconnectedClients.fetch_add(1, std::memory_order_relaxed);
        sharedState_->notifyProgress();
    }

    void handleBinary(const FastNet::Buffer& data) {
        sharedState_->clientReceivedBytes.fetch_add(data.size(), std::memory_order_relaxed);
        uint64_t latencyNs = 0;
        bool shouldFillWindow = false;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!connected_ || inFlight_ == 0 || sendTimes_.empty()) {
                return;
            }

            --inFlight_;
            const auto sentAt = sendTimes_.front();
            sendTimes_.pop_front();
            const auto now = FastNet::BenchmarkUtils::now();
            latencyNs = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(now - sentAt).count());
            shouldFillWindow = closedLoopMode_ &&
                               connected_ &&
                               sharedState_->benchmarkRunning.load(std::memory_order_acquire);
        }

        if (latencyNs != 0) {
            sharedState_->completedRoundTrips.fetch_add(1, std::memory_order_relaxed);
            sharedState_->recordLatency(latencyNs);
        }
        if (shouldFillWindow) {
            fillWindow();
        }
    }

    void fillWindow() {
        while (sharedState_->benchmarkRunning.load(std::memory_order_acquire) && trySendOne()) {
        }
    }

    FastNet::IoService& ioService_;
    FastNet::WebSocketClient client_;
    std::string url_;
    std::shared_ptr<SharedBenchmarkState> sharedState_;
    FastNet::Buffer payload_;
    size_t maxInflight_ = 1;
    bool closedLoopMode_ = true;
    bool scheduleSendsOnIoThread_ = false;
    uint32_t connectTimeoutMs_ = 0;
    FastNet::SSLConfig sslConfig_;
    std::mutex mutex_;
    bool connected_ = false;
    size_t inFlight_ = 0;
    std::deque<std::chrono::steady_clock::time_point> sendTimes_;
};

void waitForClients(const std::shared_ptr<SharedBenchmarkState>& state,
                    size_t expected,
                    std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(state->mutex);
    state->condition.wait_for(lock, timeout, [&]() {
        return state->connectedClients.load(std::memory_order_acquire) +
                   state->failedClients.load(std::memory_order_acquire) >= expected;
    });
}

void waitForServerReady(const std::shared_ptr<SharedBenchmarkState>& state,
                        size_t expected,
                        std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(state->mutex);
    state->condition.wait_for(lock, timeout, [&]() {
        return state->readyServerSessions.load(std::memory_order_acquire) >= expected;
    });
}

void waitForDisconnects(const std::shared_ptr<SharedBenchmarkState>& state,
                        size_t expected,
                        std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(state->mutex);
    state->condition.wait_for(lock, timeout, [&]() {
        return state->disconnectedClients.load(std::memory_order_acquire) >= expected;
    });
}

bool waitForIdleClients(const std::vector<std::shared_ptr<BenchmarkClient>>& clients,
                        std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        bool allIdle = true;
        for (const auto& client : clients) {
            if (client && !client->isIdle()) {
                allIdle = false;
                break;
            }
        }
        if (allIdle) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    for (const auto& client : clients) {
        if (client && !client->isIdle()) {
            return false;
        }
    }
    return true;
}

size_t benchmarkMaxConnections(size_t requestedClients) {
    const size_t burstHeadroom = (std::max)(size_t(1024), requestedClients / 2);
    if (requestedClients > (std::numeric_limits<size_t>::max)() - burstHeadroom) {
        return (std::numeric_limits<size_t>::max)();
    }
    return requestedClients + burstHeadroom;
}

void startClientWave(FastNet::IoService& ioService,
                     const std::string& url,
                     const std::shared_ptr<SharedBenchmarkState>& sharedState,
                     const BenchmarkOptions& options,
                     const FastNet::SSLConfig& clientSsl,
                     std::vector<std::shared_ptr<BenchmarkClient>>& clients,
                     size_t count) {
    clients.reserve(clients.size() + count);
    for (size_t i = 0; i < count; ++i) {
        auto client = std::make_shared<BenchmarkClient>(ioService, url, sharedState, options, clientSsl);
        clients.push_back(client);
        client->startConnect();
    }
}

bool connectBenchmarkClients(FastNet::IoService& ioService,
                             const std::string& url,
                             const std::shared_ptr<SharedBenchmarkState>& sharedState,
                             const BenchmarkOptions& options,
                             const FastNet::SSLConfig& clientSsl,
                             std::vector<std::shared_ptr<BenchmarkClient>>& clients) {
    const size_t maxExtraAttempts = (std::max)(size_t(32), options.clients / 4);
    const size_t maxLaunches = options.clients + maxExtraAttempts;
    size_t launchedClients = 0;

    while (sharedState->connectedClients.load(std::memory_order_acquire) < options.clients &&
           launchedClients < maxLaunches) {
        const uint64_t connected = sharedState->connectedClients.load(std::memory_order_acquire);
        const size_t remaining = connected >= options.clients
                                     ? 0
                                     : options.clients - static_cast<size_t>(connected);
        const size_t launchCount = (std::min)(remaining, maxLaunches - launchedClients);
        if (launchCount == 0) {
            break;
        }

        startClientWave(ioService, url, sharedState, options, clientSsl, clients, launchCount);
        launchedClients += launchCount;

        waitForClients(sharedState, launchedClients, std::chrono::milliseconds(options.connectTimeoutMs * 2ULL));
        waitForServerReady(sharedState, options.clients, std::chrono::milliseconds(options.connectTimeoutMs * 2ULL));
    }

    return sharedState->connectedClients.load(std::memory_order_acquire) >= options.clients &&
           sharedState->readyServerSessions.load(std::memory_order_acquire) >= options.clients;
}

void driveTrafficPhase(const std::vector<std::shared_ptr<BenchmarkClient>>& clients,
                       const BenchmarkOptions& options,
                       std::chrono::seconds duration) {
    if (duration.count() == 0 || clients.empty()) {
        return;
    }

    if (options.targetQps == 0) {
        for (const auto& client : clients) {
            client->startTraffic();
        }
        std::this_thread::sleep_for(duration);
        return;
    }

    const auto phaseStart = FastNet::BenchmarkUtils::now();
    const auto phaseDeadline = phaseStart + duration;
    size_t nextClientIndex = 0;
    uint64_t scheduledSends = 0;

    while (true) {
        const auto now = FastNet::BenchmarkUtils::now();
        if (now >= phaseDeadline) {
            break;
        }

        const uint64_t desiredSends = static_cast<uint64_t>(
            std::chrono::duration<double>(now - phaseStart).count() * static_cast<double>(options.targetQps));

        size_t blockedClients = 0;
        while (scheduledSends < desiredSends && blockedClients < clients.size()) {
            auto& client = clients[nextClientIndex];
            nextClientIndex = (nextClientIndex + 1) % clients.size();
            if (client->trySendOne()) {
                ++scheduledSends;
                blockedClients = 0;
            } else {
                ++blockedClients;
            }
        }

        if (scheduledSends >= desiredSends) {
            const double nextSendSeconds =
                static_cast<double>(scheduledSends + 1) / static_cast<double>(options.targetQps);
            const auto nextSendAt =
                phaseStart + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                 std::chrono::duration<double>(nextSendSeconds));
            if (nextSendAt > now) {
                const auto untilNext = nextSendAt - now;
                if (untilNext >= std::chrono::milliseconds(2)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                } else {
                    std::this_thread::yield();
                }
            } else {
                std::this_thread::yield();
            }
        } else if (blockedClients >= clients.size()) {
            std::this_thread::yield();
        } else {
            std::this_thread::yield();
        }
    }
}

FastNetExamples::BenchmarkRunResult captureRunResult(const std::shared_ptr<SharedBenchmarkState>& state,
                                                     size_t payloadBytes,
                                                     std::chrono::steady_clock::time_point benchmarkStart,
                                                     std::chrono::steady_clock::time_point benchmarkEnd) {
    FastNetExamples::BenchmarkRunResult run;
    run.seconds = std::chrono::duration<double>(benchmarkEnd - benchmarkStart).count();
    run.completed = state->completedRoundTrips.load(std::memory_order_acquire);
    run.payloadBytes = run.completed * static_cast<uint64_t>(payloadBytes);
    run.duplexBytes = run.payloadBytes * 2;
    run.totalLatencyNs = state->totalLatencyNs.load(std::memory_order_acquire);
    run.maxLatencyNs = state->maxLatencyNs.load(std::memory_order_acquire);
    run.clientErrors = state->clientErrors.load(std::memory_order_acquire);
    run.sendAttempts = state->sendAttempts.load(std::memory_order_acquire);
    run.serverReceivedMessages = state->serverReceivedMessages.load(std::memory_order_acquire);
    run.serverReceivedBytes = state->serverReceivedBytes.load(std::memory_order_acquire);
    run.clientReceivedBytes = state->clientReceivedBytes.load(std::memory_order_acquire);
    run.latencyHistogram = state->latencyHistogram.snapshot();
    return run;
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

    if (options.enableSsl && options.targetQps != 0) {
        std::cerr << "TLS target-QPS loopback is not supported by this benchmark yet.\n";
        return 1;
    }

    const FastNet::ErrorCode initCode = FastNet::initialize(options.threadCount);
    if (initCode != FastNet::ErrorCode::Success) {
        std::cerr << "FastNet initialize failed: " << FastNet::errorCodeToString(initCode) << '\n';
        return 1;
    }

    int exitCode = 0;
    {
        auto& ioService = FastNet::getGlobalIoService();
        FastNet::WebSocketServer server(ioService);
        bool serverStarted = false;
        FastNet::SSLConfig serverSsl;
        FastNet::SSLConfig clientSsl;
#ifdef FASTNET_ENABLE_SSL
        std::unique_ptr<FastNetExamples::TempTlsFiles> tlsFiles;
        if (options.enableSsl) {
            auto generatedTlsFiles = std::make_unique<FastNetExamples::TempTlsFiles>(
                FastNetExamples::makeTempTlsFiles("websocket-loopback"));
            std::string certificateError;
            if (!FastNetExamples::writeSelfSignedCertificate(*generatedTlsFiles, certificateError)) {
                std::cerr << "Failed to create loopback TLS certificate: " << certificateError << '\n';
                FastNet::cleanup();
                return 1;
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
        server.setMaxConnections(benchmarkMaxConnections(options.clients));
        server.setConnectionTimeout(0);
        server.setPingInterval(0);
        const auto sharedState = std::make_shared<SharedBenchmarkState>();
        server.setClientConnectedCallback([sharedState](FastNet::ConnectionId, const FastNet::Address&) {
            sharedState->readyServerSessions.fetch_add(1, std::memory_order_relaxed);
            sharedState->notifyProgress();
        });
        server.setOwnedBinaryCallback([&server, sharedState](FastNet::ConnectionId clientId, FastNet::Buffer&& data) {
            sharedState->serverReceivedMessages.fetch_add(1, std::memory_order_relaxed);
            sharedState->serverReceivedBytes.fetch_add(data.size(), std::memory_order_relaxed);
            const FastNet::Error result = server.sendBinaryToClient(clientId, data);
            if (result.isFailure() && !sharedState->shutdownPhase.load(std::memory_order_acquire)) {
                std::cerr << "[benchmark-ws-server] send failed: " << result.toString() << '\n';
            }
        });
        server.setServerErrorCallback([sharedState](const FastNet::Error& error) {
            if (!sharedState->shutdownPhase.load(std::memory_order_acquire)) {
                std::cerr << "[benchmark-ws-server] " << error.toString() << '\n';
            }
        });

        const FastNet::Error startResult = server.start(options.port, "127.0.0.1", serverSsl);
        if (startResult.isFailure()) {
            std::cerr << "Benchmark server start failed: " << startResult.toString() << '\n';
            exitCode = 1;
        } else {
            serverStarted = true;
            std::vector<std::shared_ptr<BenchmarkClient>> clients;
            clients.reserve(options.clients);
            const std::string scheme = options.enableSsl ? "wss://" : "ws://";
            const std::string host = "127.0.0.1";
            const uint16_t actualPort = server.getListenAddress().port;
            const std::string url = scheme + host + ":" + std::to_string(actualPort) + "/bench";
            const bool connectedAll =
                connectBenchmarkClients(ioService, url, sharedState, options, clientSsl, clients);
            const uint64_t connectedClients = sharedState->connectedClients.load(std::memory_order_acquire);
            const uint64_t readyServerSessions = sharedState->readyServerSessions.load(std::memory_order_acquire);
            const uint64_t failedClients = sharedState->failedClients.load(std::memory_order_acquire);
            if (!connectedAll) {
                std::cerr << "Failed to connect all clients. connected=" << connectedClients
                          << " server-ready=" << readyServerSessions
                          << " failed=" << failedClients << '\n';
                {
                    std::lock_guard<std::mutex> lock(sharedState->mutex);
                    if (!sharedState->firstConnectFailure.empty()) {
                        std::cerr << "First connect failure: " << sharedState->firstConnectFailure << '\n';
                    }
                }
                for (auto& client : clients) {
                    client->disconnect();
                }
                waitForDisconnects(sharedState, connectedClients, std::chrono::seconds(5));
                exitCode = 1;
            } else {
                std::cout << "FastNet WebSocket loopback benchmark\n"
                          << "  mode      : " << (options.enableSsl ? "WSS" : "WS") << '\n'
                          << "  clients   : " << options.clients << '\n'
                          << "  inflight  : " << options.maxInflight << '\n'
                          << "  traffic   : "
                          << (options.targetQps == 0 ? "closed-loop" : ("target " +
                               FastNetExamples::formatOpsRate(static_cast<double>(options.targetQps)))) << '\n'
                          << "  setup retry failures: " << failedClients << '\n'
                          << "  payload   : " << options.payloadBytes << " bytes\n"
                          << "  warmup    : " << options.warmupSeconds << " s\n"
                          << "  duration  : " << options.durationSeconds << " s\n"
                          << "  rounds    : " << options.rounds << '\n'
                          << "  settle    : " << options.settleMs << " ms\n"
                          << "  io threads: " << ioService.getThreadCount() << '\n';

                if (options.warmupSeconds != 0) {
                    sharedState->resetMetrics();
                    sharedState->benchmarkRunning.store(true, std::memory_order_release);
                    driveTrafficPhase(clients, options, std::chrono::seconds(options.warmupSeconds));
                    sharedState->benchmarkRunning.store(false, std::memory_order_release);
                    waitForIdleClients(clients, std::chrono::milliseconds(options.settleMs));
                }

                std::vector<FastNetExamples::BenchmarkRunResult> runs;
                runs.reserve(options.rounds);
                for (uint32_t round = 0; round < options.rounds; ++round) {
                    sharedState->resetMetrics();
                    const auto benchmarkStart = FastNet::BenchmarkUtils::now();
                    sharedState->benchmarkRunning.store(true, std::memory_order_release);
                    driveTrafficPhase(clients, options, std::chrono::seconds(options.durationSeconds));
                    const auto benchmarkEnd = FastNet::BenchmarkUtils::now();
                    sharedState->benchmarkRunning.store(false, std::memory_order_release);

                    auto run = captureRunResult(sharedState, options.payloadBytes, benchmarkStart, benchmarkEnd);
                    waitForIdleClients(clients, std::chrono::milliseconds(options.settleMs));
                    FastNetExamples::printRunResult(run, static_cast<size_t>(round + 1));
                    if (run.completed == 0) {
                        exitCode = 1;
                    }
                    runs.push_back(std::move(run));
                }

                sharedState->shutdownPhase.store(true, std::memory_order_release);

                if (options.enableSsl) {
                    if (serverStarted) {
                        server.stop();
                        serverStarted = false;
                    }
                } else {
                    for (auto& client : clients) {
                        client->disconnect();
                    }
                }
                waitForDisconnects(sharedState, connectedClients, std::chrono::seconds(5));
                const auto summary = FastNetExamples::summarizeRuns(runs);
                FastNetExamples::printSummary(summary);
                if (summary.successfulRounds != runs.size()) {
                    exitCode = 1;
                }
            }
        }

        if (serverStarted) {
            server.stop();
        }
    }

    FastNet::cleanup();
    return exitCode;
}
