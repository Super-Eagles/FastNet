#include "FastNet/FastNet.h"
#include "TestSupport.h"

#include <chrono>
#include <condition_variable>
#include <exception>
#include <iostream>
#include <mutex>
#include <string>

namespace {

struct ServerOptionsState {
    std::mutex mutex;
    std::condition_variable condition;
    bool connected = false;
    bool handshakeSeen = false;
    bool failed = false;
    FastNet::ConnectionId serverClientId = 0;
    std::string path;
    std::string authHeader;
    std::string failure;
};

void markFailure(ServerOptionsState& state, std::string message) {
    std::lock_guard<std::mutex> lock(state.mutex);
    if (!state.failed) {
        state.failed = true;
        state.failure = std::move(message);
    }
    state.condition.notify_all();
}

} // namespace

int main() {
    using namespace std::chrono_literals;

    try {
        FASTNET_TEST_ASSERT_EQ(FastNet::initialize(2), FastNet::ErrorCode::Success);
        struct CleanupGuard {
            ~CleanupGuard() {
                FastNet::cleanup();
            }
        } cleanupGuard;

        auto& ioService = FastNet::getGlobalIoService();
        FastNet::WebSocketServer server(ioService);
        ServerOptionsState state;

        server.setPingInterval(0);
        server.setSubprotocols({"yaos.v1"});
        server.setHandshakeResponseHeaders({{"X-FastNet-Handshake", "accepted"}});
        server.setServerErrorCallback([&](const FastNet::Error& error) {
            markFailure(state, error.toString());
        });
        server.setHandshakeCallback(
            [&](FastNet::ConnectionId,
                const FastNet::WebSocketServerHandshakeRequest& request,
                FastNet::WebSocketServerHandshakeResponse& response) {
                std::lock_guard<std::mutex> lock(state.mutex);
                state.handshakeSeen = true;
                state.path = request.path;
                state.authHeader = request.getHeader("Authorization").value_or(std::string());
                if (state.authHeader != "Bearer server-test") {
                    response.accept = false;
                    response.rejectStatusCode = 401;
                    response.rejectStatusMessage = "Unauthorized";
                    response.rejectBody = "missing token";
                }
                state.condition.notify_all();
            });
        server.setClientConnectedCallback([&](FastNet::ConnectionId clientId, const FastNet::Address&) {
            std::lock_guard<std::mutex> lock(state.mutex);
            state.serverClientId = clientId;
            state.condition.notify_all();
        });

        const FastNet::Error startResult = server.start(0, "127.0.0.1");
        FASTNET_TEST_ASSERT_MSG(startResult.isSuccess(), startResult.toString());

        FastNet::WebSocketClient client(ioService);
        client.setPingInterval(0);
        client.setConnectTimeout(3000);
        client.setSubprotocols({"yaos.v1"});
        client.setHandshakeHeaders({{"Authorization", "Bearer server-test"}});

        const std::string url = "ws://127.0.0.1:" + std::to_string(server.getListenAddress().port) + "/channel";
        FASTNET_TEST_ASSERT(client.connect(url, [&](bool success, const std::string& message) {
            std::lock_guard<std::mutex> lock(state.mutex);
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
                           (state.connected && state.handshakeSeen && state.serverClientId != 0);
                }),
                "Timed out waiting for WebSocket server option handshake");
            FASTNET_TEST_ASSERT_MSG(!state.failed, state.failure);
            FASTNET_TEST_ASSERT(state.connected);
            FASTNET_TEST_ASSERT(state.handshakeSeen);
            FASTNET_TEST_ASSERT_EQ(state.path, "/channel");
            FASTNET_TEST_ASSERT_EQ(state.authHeader, "Bearer server-test");
        }

        const auto clientProtocol = client.getAcceptedSubprotocol();
        const auto serverProtocol = server.getClientSubprotocol(state.serverClientId);
        FASTNET_TEST_ASSERT(clientProtocol.has_value());
        FASTNET_TEST_ASSERT(serverProtocol.has_value());
        FASTNET_TEST_ASSERT_EQ(*clientProtocol, "yaos.v1");
        FASTNET_TEST_ASSERT_EQ(*serverProtocol, "yaos.v1");

        client.close(1000, "done");
        server.stop();
        std::cout << "websocket server options tests passed" << '\n';
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << '\n';
        return 1;
    }
}
