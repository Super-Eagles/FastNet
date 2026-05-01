#include "FastNet/FastNet.h"
#include "TestSupport.h"

#include <chrono>
#include <condition_variable>
#include <exception>
#include <iostream>
#include <mutex>
#include <string>

namespace {

struct WebSocketState {
    std::mutex mutex;
    std::condition_variable condition;
    bool connected = false;
    bool failed = false;
    bool textReceived = false;
    bool binaryReceived = false;
    bool broadcastReceived = false;
    bool serverTextReceived = false;
    bool serverEchoQueued = false;
    bool closed = false;
    size_t inlineConnectCallbacks = 0;
    size_t persistentConnectCallbacks = 0;
    FastNet::ConnectionId serverClientId = 0;
    std::string failure;
    std::string text;
    FastNet::Buffer binary;
};

void markFailure(WebSocketState& state, std::string message) {
    std::lock_guard<std::mutex> lock(state.mutex);
    if (!state.failed) {
        state.failed = true;
        state.failure = std::move(message);
    }
    state.condition.notify_all();
}

} // namespace

int runWebSocketLoopbackTest() {
    using namespace std::chrono_literals;

    FASTNET_TEST_ASSERT_EQ(FastNet::initialize(2), FastNet::ErrorCode::Success);
    struct CleanupGuard {
        ~CleanupGuard() {
            FastNet::cleanup();
        }
    } cleanupGuard;

    auto& ioService = FastNet::getGlobalIoService();
    FastNet::WebSocketServer server(ioService);
    WebSocketState state;

    server.setMaxConnections(8);
    server.setConnectionTimeout(0);
    server.setPingInterval(0);
    server.setServerErrorCallback([&](const FastNet::Error& error) {
        markFailure(state, error.toString());
    });
    server.setClientConnectedCallback([&](FastNet::ConnectionId clientId, const FastNet::Address&) {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.serverClientId = clientId;
        state.condition.notify_all();
    });
    server.setClientDisconnectedCallback([&](FastNet::ConnectionId, uint16_t, const std::string&) {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.closed = true;
        state.condition.notify_all();
    });
    server.setMessageCallback([&](FastNet::ConnectionId clientId, const std::string& message) {
        if (message == "hello") {
            {
                std::lock_guard<std::mutex> lock(state.mutex);
                state.serverTextReceived = true;
            }
            const FastNet::Error result = server.sendTextToClient(clientId, "echo:hello");
            if (result.isFailure()) {
                markFailure(state, result.toString());
            } else {
                std::lock_guard<std::mutex> lock(state.mutex);
                state.serverEchoQueued = true;
                state.condition.notify_all();
            }
        } else if (message == "broadcast") {
            server.broadcastText("broadcast:ok");
        }
    });
    server.setOwnedBinaryCallback([&](FastNet::ConnectionId clientId, FastNet::Buffer&& data) {
        const FastNet::Error result = server.sendBinaryToClient(clientId, data);
        if (result.isFailure()) {
            markFailure(state, result.toString());
        }
    });

    const FastNet::Error startResult = server.start(0, "127.0.0.1");
    FASTNET_TEST_ASSERT_MSG(startResult.isSuccess(), startResult.toString());
    FASTNET_TEST_ASSERT(server.isRunning());
    FASTNET_TEST_ASSERT(server.getListenAddress().port != 0);

    FastNet::WebSocketClient client(ioService);
    client.setConnectTimeout(3000);
    client.setPingInterval(0);
    client.setConnectCallback([&](bool success, const std::string& message) {
        std::lock_guard<std::mutex> lock(state.mutex);
        ++state.persistentConnectCallbacks;
        if (!success) {
            state.failed = true;
            state.failure = message;
        }
        state.condition.notify_all();
    });
    client.setErrorCallback([&](FastNet::ErrorCode, const std::string& message) {
        markFailure(state, message);
    });
    client.setCloseCallback([&](uint16_t, const std::string&) {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.closed = true;
        state.condition.notify_all();
    });
    client.setMessageCallback([&](const std::string& message) {
        std::lock_guard<std::mutex> lock(state.mutex);
        if (message == "echo:hello") {
            state.textReceived = true;
            state.text = message;
        }
        if (message == "broadcast:ok") {
            state.broadcastReceived = true;
        }
        state.condition.notify_all();
    });
    client.setOwnedBinaryCallback([&](FastNet::Buffer&& data) {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.binaryReceived = true;
        state.binary = std::move(data);
        state.condition.notify_all();
    });

    const std::string url = "ws://127.0.0.1:" + std::to_string(server.getListenAddress().port) + "/";
    FASTNET_TEST_ASSERT(client.connect(url, [&](bool success, const std::string& message) {
        std::lock_guard<std::mutex> lock(state.mutex);
        ++state.inlineConnectCallbacks;
        state.connected = success;
        if (!success) {
            state.failed = true;
            state.failure = message;
        }
        state.condition.notify_all();
    }));

    {
        std::unique_lock<std::mutex> lock(state.mutex);
        FASTNET_TEST_ASSERT_MSG(
            state.condition.wait_for(lock, 3s, [&]() {
                return state.failed ||
                       (state.connected &&
                        state.inlineConnectCallbacks == 1 &&
                        state.persistentConnectCallbacks == 1);
            }),
            "Timed out waiting for WebSocket connect");
        FASTNET_TEST_ASSERT_MSG(!state.failed, state.failure);
        FASTNET_TEST_ASSERT(state.connected);
        FASTNET_TEST_ASSERT_EQ(state.inlineConnectCallbacks, static_cast<size_t>(1));
        FASTNET_TEST_ASSERT_EQ(state.persistentConnectCallbacks, static_cast<size_t>(1));
    }
    FASTNET_TEST_ASSERT(client.isConnected());
    FASTNET_TEST_ASSERT(server.getClientCount() == 1);
    FASTNET_TEST_ASSERT(state.serverClientId != 0);
    FASTNET_TEST_ASSERT(server.getClientAddress(state.serverClientId).port != 0);
    FASTNET_TEST_ASSERT(!server.getClientIds().empty());

    FASTNET_TEST_ASSERT(client.sendText("hello"));
    {
        std::unique_lock<std::mutex> lock(state.mutex);
        FASTNET_TEST_ASSERT_MSG(
            state.condition.wait_for(lock, 3s, [&]() { return state.textReceived || state.failed; }),
            std::string("Timed out waiting for WebSocket text echo; serverTextReceived=") +
                (state.serverTextReceived ? "true" : "false") +
                " serverEchoQueued=" + (state.serverEchoQueued ? "true" : "false"));
        FASTNET_TEST_ASSERT_MSG(!state.failed, state.failure);
        FASTNET_TEST_ASSERT_EQ(state.text, "echo:hello");
    }

    FastNet::Buffer binary{'b', 'i', 'n'};
    FASTNET_TEST_ASSERT(client.sendBinary(binary));
    {
        std::unique_lock<std::mutex> lock(state.mutex);
        FASTNET_TEST_ASSERT_MSG(
            state.condition.wait_for(lock, 3s, [&]() { return state.binaryReceived || state.failed; }),
            "Timed out waiting for WebSocket binary echo");
        FASTNET_TEST_ASSERT_MSG(!state.failed, state.failure);
        FASTNET_TEST_ASSERT_EQ(std::string(state.binary.begin(), state.binary.end()), "bin");
    }

    FASTNET_TEST_ASSERT(client.sendText("broadcast"));
    {
        std::unique_lock<std::mutex> lock(state.mutex);
        FASTNET_TEST_ASSERT_MSG(
            state.condition.wait_for(lock, 3s, [&]() { return state.broadcastReceived || state.failed; }),
            "Timed out waiting for WebSocket broadcast");
        FASTNET_TEST_ASSERT_MSG(!state.failed, state.failure);
    }

    client.close(1000, "done");
    {
        std::unique_lock<std::mutex> lock(state.mutex);
        state.condition.wait_for(lock, 1s, [&]() { return state.closed; });
    }
    server.disconnectClient(state.serverClientId);
    server.stop();
    FASTNET_TEST_ASSERT(!server.isRunning());

    std::cout << "websocket loopback tests passed" << '\n';
    return 0;
}

int main() {
    try {
        return runWebSocketLoopbackTest();
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "unknown websocket loopback test failure" << '\n';
        return 1;
    }
}
