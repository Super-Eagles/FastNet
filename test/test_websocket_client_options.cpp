#include "FastNet/FastNet.h"
#include "TestSupport.h"

#include <chrono>
#include <condition_variable>
#include <exception>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>

namespace {

struct HandshakeState {
    std::mutex mutex;
    std::condition_variable condition;
    bool connected = false;
    bool connectDone = false;
    bool serverValidated = false;
    bool responseSent = false;
    bool expectMaskedFrameError = false;
    bool maskedFrameRejected = false;
    bool failed = false;
    FastNet::ConnectionId serverClientId = 0;
    std::string requestText;
    std::string failure;
};

void markFailure(HandshakeState& state, std::string message) {
    std::lock_guard<std::mutex> lock(state.mutex);
    if (!state.failed) {
        state.failed = true;
        state.failure = std::move(message);
    }
    state.condition.notify_all();
}

std::optional<std::string> headerValue(const FastNet::HttpRequestView& request, std::string_view name) {
    const auto value = request.getHeader(name);
    if (!value.has_value()) {
        return std::nullopt;
    }
    return std::string(value->begin(), value->end());
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
        FastNet::TcpServer server(ioService);
        HandshakeState state;

        server.setServerErrorCallback([&](const FastNet::Error& error) {
            markFailure(state, error.toString());
        });
        server.setOwnedDataReceivedCallback([&](FastNet::ConnectionId clientId, FastNet::Buffer&& data) {
            std::string requestChunk(reinterpret_cast<const char*>(data.data()), data.size());
            std::string requestText;
            {
                std::lock_guard<std::mutex> lock(state.mutex);
                state.requestText += requestChunk;
                if (state.responseSent || state.requestText.find("\r\n\r\n") == std::string::npos) {
                    state.condition.notify_all();
                    return;
                }
                state.responseSent = true;
                state.serverClientId = clientId;
                requestText = state.requestText;
            }

            FastNet::HttpRequestView request;
            if (!FastNet::HttpParser::parseRequestHead(requestText, request)) {
                markFailure(state, "Failed to parse WebSocket handshake request");
                return;
            }

            const auto key = headerValue(request, "Sec-WebSocket-Key");
            const auto authorization = headerValue(request, "Authorization");
            const auto agent = headerValue(request, "X-YAOS-Agent");
            const auto oneShot = headerValue(request, "X-Once");
            const auto protocol = headerValue(request, "Sec-WebSocket-Protocol");
            if (!key.has_value() ||
                authorization.value_or(std::string()) != "Bearer test-token" ||
                agent.value_or(std::string()) != "studio" ||
                oneShot.value_or(std::string()) != "true" ||
                protocol.value_or(std::string()) != "yaos.v1") {
                markFailure(state, "WebSocket handshake did not include expected custom headers");
                return;
            }

            {
                std::lock_guard<std::mutex> lock(state.mutex);
                state.serverValidated = true;
                state.condition.notify_all();
            }

            std::string response =
                "HTTP/1.1 101 Switching Protocols\r\n"
                "Upgrade: websocket\r\n"
                "Connection: Upgrade\r\n"
                "Sec-WebSocket-Accept: ";
            response += FastNet::WebSocketProtocol::createAcceptKey(*key);
            response += "\r\nSec-WebSocket-Protocol: yaos.v1\r\n\r\n";

            const FastNet::Error sendResult = server.sendToClient(clientId, std::move(response));
            if (sendResult.isFailure()) {
                markFailure(state, sendResult.toString());
            }
        });

        const FastNet::Error startResult = server.start(0, "127.0.0.1");
        FASTNET_TEST_ASSERT_MSG(startResult.isSuccess(), startResult.toString());

        FastNet::WebSocketClient client(ioService);
        client.setConnectTimeout(3000);
        client.setPingInterval(0);
        client.setSubprotocols({"yaos.v1"});
        client.setHandshakeHeaders({
            {"Authorization", "Bearer test-token"},
            {"X-YAOS-Agent", "studio"},
        });
        client.setErrorCallback([&](FastNet::ErrorCode code, const std::string& message) {
            std::lock_guard<std::mutex> lock(state.mutex);
            if (code == FastNet::ErrorCode::WebSocketFrameError && state.expectMaskedFrameError) {
                state.maskedFrameRejected = true;
            } else {
                state.failed = true;
                state.failure = message;
            }
            state.condition.notify_all();
        });

        const std::string url = "ws://127.0.0.1:" + std::to_string(server.getListenAddress().port) + "/ws";
        FASTNET_TEST_ASSERT(client.connect(url, {{"X-Once", "true"}}, [&](bool success, const std::string& message) {
            std::lock_guard<std::mutex> lock(state.mutex);
            state.connectDone = true;
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
                    return state.failed || (state.connectDone && state.connected && state.serverValidated);
                }),
                "Timed out waiting for WebSocket option handshake");
            FASTNET_TEST_ASSERT_MSG(!state.failed, state.failure);
            FASTNET_TEST_ASSERT(state.connected);
            FASTNET_TEST_ASSERT(state.serverValidated);
        }

        const auto acceptedProtocol = client.getAcceptedSubprotocol();
        FASTNET_TEST_ASSERT(acceptedProtocol.has_value());
        FASTNET_TEST_ASSERT_EQ(*acceptedProtocol, "yaos.v1");

        FastNet::ConnectionId rawClientId = 0;
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            rawClientId = state.serverClientId;
            state.expectMaskedFrameError = true;
        }
        FASTNET_TEST_ASSERT(rawClientId != 0);
        const std::string maskedServerFrame =
            FastNet::WebSocketProtocol::encodeFrame("bad", FastNet::WSFrameType::Text, true);
        const FastNet::Error sendBadFrameResult = server.sendToClient(rawClientId, maskedServerFrame);
        FASTNET_TEST_ASSERT_MSG(sendBadFrameResult.isSuccess(), sendBadFrameResult.toString());

        {
            std::unique_lock<std::mutex> lock(state.mutex);
            FASTNET_TEST_ASSERT_MSG(
                state.condition.wait_for(lock, 3s, [&]() { return state.failed || state.maskedFrameRejected; }),
                "Timed out waiting for masked WebSocket server frame rejection");
            FASTNET_TEST_ASSERT_MSG(!state.failed, state.failure);
            FASTNET_TEST_ASSERT(state.maskedFrameRejected);
        }

        client.close(1000, "done");
        server.stop();
        std::cout << "websocket client options tests passed" << '\n';
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << '\n';
        return 1;
    }
}
