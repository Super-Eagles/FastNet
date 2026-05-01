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
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

struct BenchmarkOptions {
    uint16_t port = 9300;
    size_t clients = 32;
    size_t maxInflight = 1;
    size_t targetQps = 0;
    size_t payloadBytes = 1024;
    uint32_t warmupSeconds = 1;
    uint32_t durationSeconds = 5;
    uint32_t rounds = 3;
    uint32_t settleMs = 200;
    uint32_t connectTimeoutMs = 5000;
    uint32_t requestTimeoutMs = 5000;
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
        << "Usage: fastnet_http_loopback_benchmark [options]\n"
        << "  --port <value>            Listen port (default: 9300)\n"
        << "  --clients <value>         Concurrent clients (default: 32)\n"
        << "  --max-inflight <value>    Requested in-flight requests per connection (default: 1)\n"
        << "  --target-qps <value>      Target offered QPS, 0 = closed-loop (default: 0)\n"
        << "  --payload <bytes>         POST body size (default: 1024)\n"
        << "  --warmup <seconds>        Warmup duration (default: 1)\n"
        << "  --duration <seconds>      Measured duration (default: 5)\n"
        << "  --rounds <count>          Measured rounds (default: 3)\n"
        << "  --settle-ms <ms>          Idle gap between rounds (default: 200)\n"
        << "  --connect-timeout <ms>    Client connect timeout (default: 5000)\n"
        << "  --request-timeout <ms>    Per-request timeout (default: 5000)\n"
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
        } else if (std::strcmp(arg, "--request-timeout") == 0) {
            if (!parseOptionValue(value, options.requestTimeoutMs) || options.requestTimeoutMs == 0) {
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

    void resetMetrics() {
        sendAttempts.store(0, std::memory_order_release);
        serverReceivedMessages.store(0, std::memory_order_release);
        serverReceivedBytes.store(0, std::memory_order_release);
        clientReceivedBytes.store(0, std::memory_order_release);
        completedRoundTrips.store(0, std::memory_order_release);
        totalLatencyNs.store(0, std::memory_order_release);
        maxLatencyNs.store(0, std::memory_order_release);
        latencyHistogram.reset();
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
          payload_(options.payloadBytes, 'x'),
          requestHeaders_({{"Content-Type", "application/octet-stream"}}),
          closedLoopMode_(options.targetQps == 0),
          scheduleSendsOnIoThread_(options.enableSsl && options.targetQps != 0),
          connectTimeoutMs_(options.connectTimeoutMs),
          requestTimeoutMs_(options.requestTimeoutMs),
          sslConfig_(std::move(sslConfig)) {}

    void startConnect() {
        client_.setConnectTimeout(connectTimeoutMs_);
        client_.setRequestTimeout(requestTimeoutMs_);
        client_.setReadTimeout(0);
        client_.setFollowRedirects(false);
        client_.setUseCompression(false);
        client_.setSSLConfig(sslConfig_);

        std::weak_ptr<BenchmarkClient> weakSelf = shared_from_this();
        const bool started = client_.connect(url_, [weakSelf](bool success, const std::string&) {
            auto self = weakSelf.lock();
            if (!self) {
                return;
            }
            self->handleConnect(success);
        });

        if (!started) {
            sharedState_->failedClients.fetch_add(1, std::memory_order_relaxed);
            sharedState_->notifyProgress();
        }
    }

    void startTraffic() {
        if (closedLoopMode_) {
            trySendOne();
        }
    }

    void disconnect() {
        client_.disconnect();
    }

    bool trySendOne() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!connected_ || awaitingResponse_) {
                return false;
            }
            awaitingResponse_ = true;
        }

        if (scheduleSendsOnIoThread_) {
            std::weak_ptr<BenchmarkClient> weakSelf = shared_from_this();
            ioService_.post([weakSelf]() {
                auto self = weakSelf.lock();
                if (!self) {
                    return;
                }
                self->performQueuedSend();
            });
            return true;
        }
        return performQueuedSend();
    }

    bool isIdle() {
        std::lock_guard<std::mutex> lock(mutex_);
        return !awaitingResponse_;
    }

private:
    bool performQueuedSend() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!connected_ || !awaitingResponse_) {
                return false;
            }
            lastSendAt_ = FastNet::BenchmarkUtils::now();
        }

        sharedState_->sendAttempts.fetch_add(1, std::memory_order_relaxed);
        std::weak_ptr<BenchmarkClient> weakSelf = shared_from_this();
        const bool started = client_.post("/api/echo",
                                          requestHeaders_,
                                          payload_,
                                          [weakSelf](const FastNet::HttpResponse& response) {
                                              auto self = weakSelf.lock();
                                              if (!self) {
                                                  return;
                                              }
                                              self->handleResponse(response);
                                          });
        if (!started) {
            rollbackQueuedSend();
        }
        return started;
    }

    void rollbackQueuedSend() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            awaitingResponse_ = false;
        }
        sharedState_->clientErrors.fetch_add(1, std::memory_order_relaxed);
    }

    void handleConnect(bool success) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            connected_ = success;
        }
        if (success) {
            sharedState_->connectedClients.fetch_add(1, std::memory_order_relaxed);
        } else {
            sharedState_->failedClients.fetch_add(1, std::memory_order_relaxed);
        }
        sharedState_->notifyProgress();
    }

    void handleResponse(const FastNet::HttpResponse& response) {
        if (response.statusCode != 200 || response.body.size() != payload_.size()) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                awaitingResponse_ = false;
            }
            sharedState_->clientErrors.fetch_add(1, std::memory_order_relaxed);
            client_.disconnect();
            return;
        }

        sharedState_->clientReceivedBytes.fetch_add(response.body.size(), std::memory_order_relaxed);

        bool shouldSendNext = false;
        uint64_t latencyNs = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!connected_ || !awaitingResponse_) {
                return;
            }
            awaitingResponse_ = false;
            if (sharedState_->benchmarkRunning.load(std::memory_order_acquire)) {
                const auto now = FastNet::BenchmarkUtils::now();
                latencyNs = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(now - lastSendAt_).count());
                shouldSendNext = true;
            }
        }

        if (latencyNs != 0) {
            sharedState_->completedRoundTrips.fetch_add(1, std::memory_order_relaxed);
            sharedState_->recordLatency(latencyNs);
        }
        if (shouldSendNext && closedLoopMode_) {
            trySendOne();
        }
    }

    FastNet::IoService& ioService_;
    FastNet::HttpClient client_;
    std::string url_;
    std::shared_ptr<SharedBenchmarkState> sharedState_;
    std::string payload_;
    FastNet::RequestHeaders requestHeaders_;
    bool closedLoopMode_ = true;
    bool scheduleSendsOnIoThread_ = false;
    uint32_t connectTimeoutMs_ = 0;
    uint32_t requestTimeoutMs_ = 0;
    FastNet::SSLConfig sslConfig_;
    std::mutex mutex_;
    bool connected_ = false;
    bool awaitingResponse_ = false;
    std::chrono::steady_clock::time_point lastSendAt_{};
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
    std::cout.setf(std::ios::unitbuf);

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
        FastNet::HttpServer server(ioService);
        bool serverStarted = false;
        FastNet::SSLConfig serverSsl;
        FastNet::SSLConfig clientSsl;
#ifdef FASTNET_ENABLE_SSL
        std::unique_ptr<FastNetExamples::TempTlsFiles> tlsFiles;
        if (options.enableSsl) {
            auto generatedTlsFiles =
                std::make_unique<FastNetExamples::TempTlsFiles>(FastNetExamples::makeTempTlsFiles("http-loopback"));
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
        server.setMaxConnections(options.clients + 16);
        server.setConnectionTimeout(0);
        server.setRequestTimeout(0);
        server.setWriteTimeout(0);
        server.setMaxRequestSize((std::max<size_t>)(options.payloadBytes * 2, 4096));
        const auto sharedState = std::make_shared<SharedBenchmarkState>();

        server.registerPost("/api/echo", [sharedState](const FastNet::HttpRequest& request, FastNet::HttpResponse& response) {
            sharedState->serverReceivedMessages.fetch_add(1, std::memory_order_relaxed);
            sharedState->serverReceivedBytes.fetch_add(request.body.size(), std::memory_order_relaxed);
            response.statusCode = 200;
            response.statusMessage = "OK";
            response.headers["Content-Type"] = "application/octet-stream";
            response.body = request.body;
        });

        server.setServerErrorCallback([sharedState](const FastNet::Error& error) {
            if (!sharedState->shutdownPhase.load(std::memory_order_acquire)) {
                std::cerr << "[benchmark-http-server] " << error.toString() << '\n';
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
            const std::string scheme = options.enableSsl ? "https://" : "http://";
            const std::string host = "127.0.0.1";
            const std::string url = scheme + host + ":" + std::to_string(options.port);
            for (size_t i = 0; i < options.clients; ++i) {
                auto client = std::make_shared<BenchmarkClient>(ioService, url, sharedState, options, clientSsl);
                clients.push_back(client);
                client->startConnect();
            }

            waitForClients(sharedState, options.clients, std::chrono::milliseconds(options.connectTimeoutMs * 2ULL));
            const uint64_t connectedClients = sharedState->connectedClients.load(std::memory_order_acquire);
            const uint64_t failedClients = sharedState->failedClients.load(std::memory_order_acquire);
            if (connectedClients != options.clients || failedClients != 0) {
                std::cerr << "Failed to connect all clients. connected=" << connectedClients
                          << " failed=" << failedClients << '\n';
                for (auto& client : clients) {
                    client->disconnect();
                }
                exitCode = 1;
            } else {
                sharedState->readyServerSessions.store(connectedClients, std::memory_order_release);
                sharedState->notifyProgress();
                waitForServerReady(sharedState, options.clients, std::chrono::milliseconds(100));

                std::cout << "FastNet HTTP loopback benchmark\n"
                          << "  mode      : " << (options.enableSsl ? "HTTPS" : "HTTP") << '\n'
                          << "  clients   : " << options.clients << '\n'
                          << "  inflight  : 1";
                if (options.maxInflight != 1) {
                    std::cout << " (requested " << options.maxInflight << ", HTTP/1.1 effective 1)";
                }
                std::cout << '\n'
                          << "  traffic   : "
                          << (options.targetQps == 0 ? "closed-loop" : ("target " +
                              FastNetExamples::formatOpsRate(static_cast<double>(options.targetQps)))) << '\n'
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

                if (options.enableSsl && serverStarted) {
                    server.stop();
                    serverStarted = false;
                }
                for (auto& client : clients) {
                    client->disconnect();
                }
                waitForDisconnects(sharedState, connectedClients, std::chrono::seconds(1));
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
