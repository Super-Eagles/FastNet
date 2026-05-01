#include "FastNet/FastNet.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <string>
#include <thread>

namespace {

struct Options {
    uint16_t port = 19400;
    size_t maxInflight = 64;
    size_t payloadBytes = 1024;
    uint32_t warmupSeconds = 0;
    uint32_t durationSeconds = 2;
    size_t threadCount = 0;
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

template<typename T>
bool parseValue(const char* text, T& value) {
    unsigned long long parsed = 0;
    if (!parseUnsigned(text, parsed) ||
        parsed > static_cast<unsigned long long>((std::numeric_limits<T>::max)())) {
        return false;
    }
    value = static_cast<T>(parsed);
    return true;
}

bool parseArgs(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (std::strcmp(arg, "--help") == 0) {
            std::cout
                << "Usage: fastnet_udp_loopback_benchmark [options]\n"
                << "  --port <value>          Server UDP port (default: 19400)\n"
                << "  --max-inflight <value>  Client in-flight datagrams (default: 64)\n"
                << "  --payload <bytes>       Echo payload size (default: 1024)\n"
                << "  --warmup <seconds>      Warmup duration (default: 0)\n"
                << "  --duration <seconds>    Measured duration (default: 2)\n"
                << "  --threads <value>       FastNet IO threads, 0 = auto (default: 0)\n";
            return false;
        }
        if (i + 1 >= argc) {
            std::cerr << "Missing value for argument: " << arg << '\n';
            return false;
        }
        const char* value = argv[++i];
        if (std::strcmp(arg, "--port") == 0) {
            if (!parseValue(value, options.port)) {
                return false;
            }
        } else if (std::strcmp(arg, "--max-inflight") == 0) {
            if (!parseValue(value, options.maxInflight) || options.maxInflight == 0) {
                return false;
            }
        } else if (std::strcmp(arg, "--payload") == 0) {
            if (!parseValue(value, options.payloadBytes) || options.payloadBytes == 0) {
                return false;
            }
        } else if (std::strcmp(arg, "--warmup") == 0) {
            if (!parseValue(value, options.warmupSeconds)) {
                return false;
            }
        } else if (std::strcmp(arg, "--duration") == 0) {
            if (!parseValue(value, options.durationSeconds) || options.durationSeconds == 0) {
                return false;
            }
        } else if (std::strcmp(arg, "--threads") == 0) {
            if (!parseValue(value, options.threadCount)) {
                return false;
            }
        } else {
            std::cerr << "Unknown argument: " << arg << '\n';
            return false;
        }
    }
    return true;
}

struct State {
    std::atomic<bool> running{false};
    std::atomic<uint64_t> sent{0};
    std::atomic<uint64_t> completed{0};
    std::atomic<uint64_t> serverReceived{0};
    std::atomic<uint64_t> errors{0};
    std::mutex mutex;
    std::condition_variable condition;
};

void sendOne(FastNet::UdpSocket& client,
             const FastNet::Address& serverAddress,
             const FastNet::Buffer& payload,
             State& state) {
    if (client.sendTo(serverAddress, payload)) {
        state.sent.fetch_add(1, std::memory_order_relaxed);
    } else {
        state.errors.fetch_add(1, std::memory_order_relaxed);
    }
}

void runPhase(FastNet::UdpSocket& client,
              const FastNet::Address& serverAddress,
              const FastNet::Buffer& payload,
              State& state,
              const Options& options,
              std::chrono::seconds duration) {
    state.sent.store(0, std::memory_order_release);
    state.completed.store(0, std::memory_order_release);
    state.serverReceived.store(0, std::memory_order_release);
    state.errors.store(0, std::memory_order_release);
    state.running.store(true, std::memory_order_release);

    for (size_t index = 0; index < options.maxInflight; ++index) {
        sendOne(client, serverAddress, payload, state);
    }

    std::this_thread::sleep_for(duration);
    state.running.store(false, std::memory_order_release);

    std::unique_lock<std::mutex> lock(state.mutex);
    state.condition.wait_for(lock, std::chrono::milliseconds(200));
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parseArgs(argc, argv, options)) {
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
        FastNet::UdpSocket server(ioService);
        FastNet::UdpSocket client(ioService);
        State state;
        const FastNet::Buffer payload(options.payloadBytes, static_cast<std::uint8_t>('u'));

        server.setErrorCallback([&](FastNet::ErrorCode, const std::string& message) {
            ++state.errors;
            std::cerr << "[udp-server] " << message << '\n';
        });
        client.setErrorCallback([&](FastNet::ErrorCode, const std::string& message) {
            ++state.errors;
            std::cerr << "[udp-client] " << message << '\n';
        });
        server.setDataReceivedCallback([&](const FastNet::Address& sender, const FastNet::Buffer& data) {
            state.serverReceived.fetch_add(1, std::memory_order_relaxed);
            server.sendTo(sender, data);
        });
        client.setDataReceivedCallback([&](const FastNet::Address&, const FastNet::Buffer&) {
            state.completed.fetch_add(1, std::memory_order_relaxed);
            if (state.running.load(std::memory_order_acquire)) {
                sendOne(client, server.getLocalAddress(), payload, state);
            } else {
                std::lock_guard<std::mutex> lock(state.mutex);
                state.condition.notify_all();
            }
        });

        if (!server.bind(options.port, "127.0.0.1") ||
            !client.bind(0, "127.0.0.1") ||
            !server.startReceive() ||
            !client.startReceive()) {
            std::cerr << "Failed to start UDP loopback sockets\n";
            exitCode = 1;
        } else {
            std::cout << "FastNet UDP loopback benchmark\n"
                      << "  payload   : " << options.payloadBytes << " bytes\n"
                      << "  inflight  : " << options.maxInflight << '\n'
                      << "  warmup    : " << options.warmupSeconds << " s\n"
                      << "  duration  : " << options.durationSeconds << " s\n"
                      << "  io threads: " << ioService.getThreadCount() << '\n';

            if (options.warmupSeconds != 0) {
                runPhase(client, server.getLocalAddress(), payload, state, options, std::chrono::seconds(options.warmupSeconds));
            }

            const auto start = std::chrono::steady_clock::now();
            runPhase(client, server.getLocalAddress(), payload, state, options, std::chrono::seconds(options.durationSeconds));
            const auto end = std::chrono::steady_clock::now();
            const double seconds = std::chrono::duration<double>(end - start).count();
            const uint64_t completed = state.completed.load(std::memory_order_acquire);
            const uint64_t sent = state.sent.load(std::memory_order_acquire);
            const uint64_t errors = state.errors.load(std::memory_order_acquire);
            const double qps = seconds > 0.0 ? static_cast<double>(completed) / seconds : 0.0;
            const double mbps = seconds > 0.0
                                    ? (static_cast<double>(completed * options.payloadBytes * 2) / seconds / 1024.0 / 1024.0)
                                    : 0.0;
            std::cout << "  round trips: " << completed << '\n'
                      << "  sent       : " << sent << '\n'
                      << "  throughput : " << std::fixed << std::setprecision(2) << qps << " QTPS\n"
                      << "  duplex bw  : " << std::fixed << std::setprecision(2) << mbps << " MB/s\n"
                      << "  errors     : " << errors << '\n';
            if (completed == 0 || errors != 0) {
                exitCode = 1;
            }
        }

        client.stopReceive();
        server.stopReceive();
    }

    FastNet::cleanup();
    return exitCode;
}
