#include "BenchmarkCommon.h"
#include "FastNet/FastNet.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef FASTNET_ENABLE_SSL
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#endif

namespace {

struct BenchmarkOptions {
    uint16_t port = 9100;
    size_t clients = 64;
    size_t maxInflight = 1;
    size_t targetQps = 0;
    size_t payloadBytes = 1024;
    uint32_t warmupSeconds = 2;
    uint32_t durationSeconds = 10;
    uint32_t rounds = 3;
    uint32_t settleMs = 200;
    uint32_t connectTimeoutMs = 5000;
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
        << "Usage: fastnet_tcp_loopback_benchmark [options]\n"
        << "  --port <value>            Listen port (default: 9100)\n"
        << "  --clients <value>         Concurrent clients (default: 64)\n"
        << "  --max-inflight <value>    Max in-flight requests per connection (default: 1)\n"
        << "  --target-qps <value>      Target offered QPS, 0 = closed-loop (default: 0)\n"
        << "  --payload <bytes>         Echo payload size (default: 1024)\n"
        << "  --warmup <seconds>        Warmup duration (default: 2)\n"
        << "  --duration <seconds>      Measured duration (default: 10)\n"
        << "  --rounds <count>          Measured rounds (default: 3)\n"
        << "  --settle-ms <ms>          Idle gap between rounds (default: 200)\n"
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

#ifdef FASTNET_ENABLE_SSL
struct TempTlsFiles {
    std::filesystem::path directory;
    std::filesystem::path certificate;
    std::filesystem::path privateKey;

    TempTlsFiles() = default;
    TempTlsFiles(const TempTlsFiles&) = delete;
    TempTlsFiles& operator=(const TempTlsFiles&) = delete;
    TempTlsFiles(TempTlsFiles&& other) noexcept
        : directory(std::move(other.directory)),
          certificate(std::move(other.certificate)),
          privateKey(std::move(other.privateKey)) {
        other.directory.clear();
        other.certificate.clear();
        other.privateKey.clear();
    }
    TempTlsFiles& operator=(TempTlsFiles&& other) noexcept {
        if (this == &other) {
            return *this;
        }
        directory = std::move(other.directory);
        certificate = std::move(other.certificate);
        privateKey = std::move(other.privateKey);
        other.directory.clear();
        other.certificate.clear();
        other.privateKey.clear();
        return *this;
    }

    ~TempTlsFiles() {
        std::error_code ec;
        if (!directory.empty()) {
            std::filesystem::remove_all(directory, ec);
        }
    }
};

TempTlsFiles makeTempTlsFiles() {
    const auto baseDir = std::filesystem::current_path() / "tmp";
    std::error_code ec;
    std::filesystem::create_directories(baseDir, ec);

    const auto uniqueSuffix =
        std::to_string(static_cast<long long>(std::chrono::steady_clock::now().time_since_epoch().count()));
    TempTlsFiles files;
    files.directory = baseDir / ("tcp-loopback-benchmark-" + uniqueSuffix);
    std::filesystem::create_directories(files.directory, ec);
    files.certificate = files.directory / "server.crt";
    files.privateKey = files.directory / "server.key";
    return files;
}

bool writeSelfSignedCertificate(const TempTlsFiles& files, std::string& errorMessage) {
    EVP_PKEY_CTX* keyContext = nullptr;
    EVP_PKEY* key = nullptr;
    X509* certificate = nullptr;
    BIO* certBio = nullptr;
    BIO* keyBio = nullptr;
    bool success = false;

    do {
        keyContext = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
        if (keyContext == nullptr) {
            errorMessage = "Failed to allocate EVP_PKEY_CTX";
            break;
        }
        if (EVP_PKEY_keygen_init(keyContext) <= 0) {
            errorMessage = "EVP_PKEY_keygen_init failed";
            break;
        }
        if (EVP_PKEY_CTX_set_rsa_keygen_bits(keyContext, 2048) <= 0) {
            errorMessage = "EVP_PKEY_CTX_set_rsa_keygen_bits failed";
            break;
        }
        if (EVP_PKEY_keygen(keyContext, &key) <= 0 || key == nullptr) {
            errorMessage = "EVP_PKEY_keygen failed";
            break;
        }

        certificate = X509_new();
        if (certificate == nullptr) {
            errorMessage = "Failed to allocate X509 certificate";
            break;
        }
        if (X509_set_version(certificate, 2) != 1) {
            errorMessage = "X509_set_version failed";
            break;
        }
        if (ASN1_INTEGER_set(X509_get_serialNumber(certificate), 1) != 1) {
            errorMessage = "Failed to set certificate serial number";
            break;
        }
        if (X509_gmtime_adj(X509_get_notBefore(certificate), 0) == nullptr ||
            X509_gmtime_adj(X509_get_notAfter(certificate), 24 * 60 * 60) == nullptr) {
            errorMessage = "Failed to set certificate validity window";
            break;
        }
        if (X509_set_pubkey(certificate, key) != 1) {
            errorMessage = "X509_set_pubkey failed";
            break;
        }

        X509_NAME* subject = X509_get_subject_name(certificate);
        if (subject == nullptr ||
            X509_NAME_add_entry_by_txt(
                subject, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>("localhost"), -1, -1, 0) != 1) {
            errorMessage = "Failed to set certificate subject";
            break;
        }
        if (X509_set_issuer_name(certificate, subject) != 1) {
            errorMessage = "Failed to set certificate issuer";
            break;
        }
        if (X509_sign(certificate, key, EVP_sha256()) <= 0) {
            errorMessage = "X509_sign failed";
            break;
        }

        certBio = BIO_new(BIO_s_mem());
        if (certBio == nullptr || PEM_write_bio_X509(certBio, certificate) != 1) {
            errorMessage = "Failed to write certificate PEM";
            break;
        }

        keyBio = BIO_new(BIO_s_mem());
        if (keyBio == nullptr || PEM_write_bio_PrivateKey(keyBio, key, nullptr, nullptr, 0, nullptr, nullptr) != 1) {
            errorMessage = "Failed to write private key PEM";
            break;
        }

        BUF_MEM* certBuffer = nullptr;
        BUF_MEM* keyBuffer = nullptr;
        BIO_get_mem_ptr(certBio, &certBuffer);
        BIO_get_mem_ptr(keyBio, &keyBuffer);
        if (certBuffer == nullptr || keyBuffer == nullptr) {
            errorMessage = "Failed to extract certificate PEM buffer";
            break;
        }

        std::ofstream certStream(files.certificate, std::ios::binary | std::ios::trunc);
        if (!certStream.write(certBuffer->data, static_cast<std::streamsize>(certBuffer->length))) {
            errorMessage = "Failed to persist certificate PEM";
            break;
        }

        std::ofstream keyStream(files.privateKey, std::ios::binary | std::ios::trunc);
        if (!keyStream.write(keyBuffer->data, static_cast<std::streamsize>(keyBuffer->length))) {
            errorMessage = "Failed to persist private key PEM";
            break;
        }

        success = true;
    } while (false);

    if (certBio != nullptr) {
        BIO_free(certBio);
    }
    if (keyBio != nullptr) {
        BIO_free(keyBio);
    }
    if (certificate != nullptr) {
        X509_free(certificate);
    }
    if (key != nullptr) {
        EVP_PKEY_free(key);
    }
    if (keyContext != nullptr) {
        EVP_PKEY_CTX_free(keyContext);
    }
    return success;
}
#endif

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
                    FastNet::Address serverAddress,
                    std::shared_ptr<SharedBenchmarkState> sharedState,
                    const BenchmarkOptions& options,
                    FastNet::SSLConfig sslConfig)
        : ioService_(ioService),
          client_(ioService),
          serverAddress_(std::move(serverAddress)),
          sharedState_(std::move(sharedState)),
          payload_(options.payloadBytes, 'x'),
          sharedPayload_(std::make_shared<const std::string>(payload_)),
          maxInflight_((std::max)(static_cast<size_t>(1), options.maxInflight)),
          closedLoopMode_(options.targetQps == 0),
          scheduleSendsOnIoThread_(options.enableSsl && options.targetQps != 0),
          connectTimeoutMs_(options.connectTimeoutMs),
          sslConfig_(std::move(sslConfig)) {}

    void startConnect() {
        std::weak_ptr<BenchmarkClient> weakSelf = shared_from_this();
        if (sslConfig_.enableSSL) {
            client_.setDataReceivedCallback([weakSelf](const FastNet::Buffer& data) {
                auto self = weakSelf.lock();
                if (!self) {
                    return;
                }
                self->handleData(data.size());
            });
        } else {
            client_.setSharedDataReceivedCallback([weakSelf](const std::shared_ptr<const FastNet::Buffer>& data) {
                auto self = weakSelf.lock();
                if (!self) {
                    return;
                }
                self->handleData(data->size());
            });
        }
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

    void startTraffic() {
        if (closedLoopMode_) {
            fillWindow();
        }
    }

    void disconnect() {
        client_.disconnectAfterPendingWrites();
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
        const bool sent = sslConfig_.enableSSL ? client_.send(std::string(payload_)) : client_.send(sharedPayload_);
        if (!sent) {
            rollbackQueuedSend();
        }
        return sent;
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

    void handleDisconnect() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            connected_ = false;
            receivedBytes_ = 0;
            inFlight_ = 0;
            sendTimes_.clear();
        }
        sharedState_->disconnectedClients.fetch_add(1, std::memory_order_relaxed);
        sharedState_->notifyProgress();
    }

    void handleData(size_t bytesReceived) {
        sharedState_->clientReceivedBytes.fetch_add(bytesReceived, std::memory_order_relaxed);
        std::vector<uint64_t> latencies;
        bool shouldFillWindow = false;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!connected_) {
                return;
            }

            receivedBytes_ += bytesReceived;
            latencies.reserve((receivedBytes_ / payload_.size()) + 1);
            const auto now = FastNet::BenchmarkUtils::now();
            while (receivedBytes_ >= payload_.size() && inFlight_ != 0 && !sendTimes_.empty()) {
                receivedBytes_ -= payload_.size();
                --inFlight_;
                const auto sentAt = sendTimes_.front();
                sendTimes_.pop_front();
                latencies.push_back(static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(now - sentAt).count()));
            }
            shouldFillWindow = closedLoopMode_ &&
                               connected_ &&
                               sharedState_->benchmarkRunning.load(std::memory_order_acquire);
        }

        for (uint64_t latencyNs : latencies) {
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
    FastNet::TcpClient client_;
    FastNet::Address serverAddress_;
    std::shared_ptr<SharedBenchmarkState> sharedState_;
    std::string payload_;
    std::shared_ptr<const std::string> sharedPayload_;
    size_t maxInflight_ = 1;
    bool closedLoopMode_ = true;
    bool scheduleSendsOnIoThread_ = false;
    uint32_t connectTimeoutMs_ = 0;
    FastNet::SSLConfig sslConfig_;
    std::mutex mutex_;
    bool connected_ = false;
    size_t inFlight_ = 0;
    size_t receivedBytes_ = 0;
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
        FastNet::TcpServer server(ioService);
        bool serverStarted = false;
        FastNet::SSLConfig serverSsl;
        FastNet::SSLConfig clientSsl;
#ifdef FASTNET_ENABLE_SSL
        std::unique_ptr<TempTlsFiles> tlsFiles;
        if (options.enableSsl) {
            auto generatedTlsFiles = std::make_unique<TempTlsFiles>(makeTempTlsFiles());
            std::string certificateError;
            if (!writeSelfSignedCertificate(*generatedTlsFiles, certificateError)) {
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
        server.setReadTimeout(0);
        server.setWriteTimeout(0);
        const auto sharedState = std::make_shared<SharedBenchmarkState>();
        server.setClientConnectedCallback([sharedState](FastNet::ConnectionId, const FastNet::Address&) {
            sharedState->readyServerSessions.fetch_add(1, std::memory_order_relaxed);
            sharedState->notifyProgress();
        });
        if (options.enableSsl) {
            server.setOwnedDataReceivedCallback([&server, sharedState](FastNet::ConnectionId clientId, FastNet::Buffer&& data) {
                sharedState->serverReceivedMessages.fetch_add(1, std::memory_order_relaxed);
                sharedState->serverReceivedBytes.fetch_add(data.size(), std::memory_order_relaxed);
                const FastNet::Error result = server.sendToClient(clientId, std::move(data));
                if (result.isFailure() && !sharedState->shutdownPhase.load(std::memory_order_acquire)) {
                    std::cerr << "[benchmark-server] send failed: " << result.toString() << '\n';
                }
            });
        } else {
            server.setSharedDataReceivedCallback(
                [&server, sharedState](FastNet::ConnectionId clientId, const std::shared_ptr<const FastNet::Buffer>& data) {
                    sharedState->serverReceivedMessages.fetch_add(1, std::memory_order_relaxed);
                    sharedState->serverReceivedBytes.fetch_add(data->size(), std::memory_order_relaxed);
                    const FastNet::Error result = server.sendToClient(clientId, data);
                    if (result.isFailure() && !sharedState->shutdownPhase.load(std::memory_order_acquire)) {
                        std::cerr << "[benchmark-server] send failed: " << result.toString() << '\n';
                    }
                });
        }
        server.setServerErrorCallback([sharedState](const FastNet::Error& error) {
            if (!sharedState->shutdownPhase.load(std::memory_order_acquire)) {
                std::cerr << "[benchmark-server] " << error.toString() << '\n';
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
            const FastNet::Address serverAddress = server.getListenAddress();
            for (size_t i = 0; i < options.clients; ++i) {
                auto client =
                    std::make_shared<BenchmarkClient>(ioService, serverAddress, sharedState, options, clientSsl);
                clients.push_back(client);
                client->startConnect();
            }

            waitForClients(sharedState, options.clients, std::chrono::milliseconds(options.connectTimeoutMs * 2ULL));
            if (!options.enableSsl) {
                waitForServerReady(sharedState, options.clients, std::chrono::milliseconds(options.connectTimeoutMs * 2ULL));
            }
            const uint64_t connectedClients = sharedState->connectedClients.load(std::memory_order_acquire);
            const uint64_t readyServerSessions = sharedState->readyServerSessions.load(std::memory_order_acquire);
            const uint64_t failedClients = sharedState->failedClients.load(std::memory_order_acquire);
            const bool allClientsReady = connectedClients == options.clients && failedClients == 0;
            const bool allServerSessionsReady = options.enableSsl || readyServerSessions == options.clients;
            if (!allClientsReady || !allServerSessionsReady) {
                std::cerr << "Failed to connect all clients. connected=" << connectedClients
                          << " server-ready=" << readyServerSessions
                          << " failed=" << failedClients << '\n';
                for (auto& client : clients) {
                    client->disconnect();
                }
                waitForDisconnects(sharedState, connectedClients, std::chrono::seconds(2));
                exitCode = 1;
            } else {
                std::cout << "FastNet TCP loopback benchmark\n"
                          << "  mode      : " << (options.enableSsl ? "TLS" : "Plain") << '\n'
                          << "  clients   : " << options.clients << '\n'
                          << "  inflight  : " << options.maxInflight << '\n'
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
                if (options.enableSsl) {
                    server.stop();
                    serverStarted = false;
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
