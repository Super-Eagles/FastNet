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
    bool rawConnectDone = false;
    bool rawConnected = false;
    bool expectHandshakeError = false;
    bool invalidKeyRejected = false;
    bool invalidKeyDisconnected = false;
    bool expectFrameError = false;
    bool unmaskedFrameRejected = false;
    bool rawFrameHandshakeAccepted = false;
    bool failed = false;
    FastNet::ConnectionId serverClientId = 0;
    std::string path;
    std::string authHeader;
    std::string invalidKeyResponse;
    std::string rawFrameResponse;
    std::string failure;
};

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
            std::lock_guard<std::mutex> lock(state.mutex);
            if (error.getCode() == FastNet::ErrorCode::WebSocketHandshakeError && state.expectHandshakeError) {
                state.invalidKeyRejected = true;
            } else if (error.getCode() == FastNet::ErrorCode::WebSocketFrameError && state.expectFrameError) {
                state.unmaskedFrameRejected = true;
            } else if (!state.failed) {
                state.failed = true;
                state.failure = error.toString();
            }
            state.condition.notify_all();
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

        FastNet::TcpClient invalidKeyClient(ioService);
        invalidKeyClient.setConnectTimeout(3000);
        invalidKeyClient.setDisconnectCallback([&](const std::string&) {
            std::lock_guard<std::mutex> lock(state.mutex);
            state.invalidKeyDisconnected = true;
            state.condition.notify_all();
        });
        invalidKeyClient.setOwnedDataReceivedCallback([&](FastNet::Buffer&& data) {
            std::lock_guard<std::mutex> lock(state.mutex);
            state.invalidKeyResponse.append(reinterpret_cast<const char*>(data.data()), data.size());
            state.condition.notify_all();
        });
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            state.rawConnectDone = false;
            state.rawConnected = false;
            state.expectHandshakeError = true;
            state.invalidKeyRejected = false;
            state.invalidKeyDisconnected = false;
            state.invalidKeyResponse.clear();
        }
        FASTNET_TEST_ASSERT(invalidKeyClient.connect("127.0.0.1",
                                                     server.getListenAddress().port,
                                                     [&](bool success, const std::string& message) {
                                                         std::lock_guard<std::mutex> lock(state.mutex);
                                                         state.rawConnectDone = true;
                                                         state.rawConnected = success;
                                                         if (!success) {
                                                             state.failed = true;
                                                             state.failure = message;
                                                         }
                                                         state.condition.notify_all();
                                                     }));
        {
            std::unique_lock<std::mutex> lock(state.mutex);
            FASTNET_TEST_ASSERT_MSG(
                state.condition.wait_for(lock, 3s, [&]() { return state.failed || state.rawConnectDone; }),
                "Timed out waiting for invalid-key TCP client connect");
            FASTNET_TEST_ASSERT_MSG(!state.failed, state.failure);
            FASTNET_TEST_ASSERT(state.rawConnected);
        }
        const std::string invalidKeyRequest =
            "GET /invalid HTTP/1.1\r\n"
            "Host: 127.0.0.1\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Authorization: Bearer server-test\r\n"
            "Sec-WebSocket-Key: bad-key\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "\r\n";
        FASTNET_TEST_ASSERT(invalidKeyClient.send(invalidKeyRequest));
        {
            std::unique_lock<std::mutex> lock(state.mutex);
            FASTNET_TEST_ASSERT_MSG(
                state.condition.wait_for(lock, 3s, [&]() {
                    return state.failed ||
                           state.invalidKeyRejected ||
                           state.invalidKeyDisconnected ||
                           !state.invalidKeyResponse.empty();
                }),
                "Timed out waiting for invalid WebSocket key rejection");
            FASTNET_TEST_ASSERT_MSG(!state.failed, state.failure);
            FASTNET_TEST_ASSERT(state.invalidKeyResponse.find("101 Switching Protocols") == std::string::npos);
            FASTNET_TEST_ASSERT(state.invalidKeyRejected || state.invalidKeyDisconnected);
        }
        invalidKeyClient.disconnect();

        FastNet::TcpClient unmaskedFrameClient(ioService);
        unmaskedFrameClient.setConnectTimeout(3000);
        unmaskedFrameClient.setOwnedDataReceivedCallback([&](FastNet::Buffer&& data) {
            std::string chunk(reinterpret_cast<const char*>(data.data()), data.size());
            bool sendUnmaskedFrame = false;
            {
                std::lock_guard<std::mutex> lock(state.mutex);
                state.rawFrameResponse += chunk;
                if (!state.rawFrameHandshakeAccepted &&
                    state.rawFrameResponse.find("\r\n\r\n") != std::string::npos) {
                    state.rawFrameHandshakeAccepted = true;
                    sendUnmaskedFrame = true;
                }
                state.condition.notify_all();
            }
            if (sendUnmaskedFrame) {
                unmaskedFrameClient.send(
                    FastNet::WebSocketProtocol::encodeFrame("bad", FastNet::WSFrameType::Text, false));
            }
        });
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            state.rawConnectDone = false;
            state.rawConnected = false;
            state.expectHandshakeError = false;
            state.expectFrameError = true;
            state.unmaskedFrameRejected = false;
            state.rawFrameHandshakeAccepted = false;
            state.rawFrameResponse.clear();
        }
        FASTNET_TEST_ASSERT(unmaskedFrameClient.connect("127.0.0.1",
                                                        server.getListenAddress().port,
                                                        [&](bool success, const std::string& message) {
                                                            std::lock_guard<std::mutex> lock(state.mutex);
                                                            state.rawConnectDone = true;
                                                            state.rawConnected = success;
                                                            if (!success) {
                                                                state.failed = true;
                                                                state.failure = message;
                                                            }
                                                            state.condition.notify_all();
                                                        }));
        {
            std::unique_lock<std::mutex> lock(state.mutex);
            FASTNET_TEST_ASSERT_MSG(
                state.condition.wait_for(lock, 3s, [&]() { return state.failed || state.rawConnectDone; }),
                "Timed out waiting for unmasked-frame TCP client connect");
            FASTNET_TEST_ASSERT_MSG(!state.failed, state.failure);
            FASTNET_TEST_ASSERT(state.rawConnected);
        }
        const std::string validRawHandshake =
            "GET /raw HTTP/1.1\r\n"
            "Host: 127.0.0.1\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Authorization: Bearer server-test\r\n"
            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "\r\n";
        FASTNET_TEST_ASSERT(unmaskedFrameClient.send(validRawHandshake));
        {
            std::unique_lock<std::mutex> lock(state.mutex);
            FASTNET_TEST_ASSERT_MSG(
                state.condition.wait_for(lock, 3s, [&]() { return state.failed || state.unmaskedFrameRejected; }),
                "Timed out waiting for unmasked WebSocket client frame rejection");
            FASTNET_TEST_ASSERT_MSG(!state.failed, state.failure);
            FASTNET_TEST_ASSERT(state.rawFrameHandshakeAccepted);
            FASTNET_TEST_ASSERT(state.unmaskedFrameRejected);
        }
        unmaskedFrameClient.disconnect();

        client.close(1000, "done");
        server.stop();
        std::cout << "websocket server options tests passed" << '\n';
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << '\n';
        return 1;
    }
}
