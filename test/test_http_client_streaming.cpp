#include "FastNet/FastNet.h"
#include "TestSupport.h"

#include <chrono>
#include <condition_variable>
#include <exception>
#include <iostream>
#include <mutex>
#include <string>

namespace {

struct StreamState {
    std::mutex mutex;
    std::condition_variable condition;
    bool connectDone = false;
    bool connected = false;
    bool headersSeen = false;
    bool complete = false;
    bool requestSeen = false;
    bool failed = false;
    int statusCode = 0;
    std::string chunks;
    std::string requestText;
    std::string failure;
};

void markFailure(StreamState& state, std::string message) {
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
        FastNet::TcpServer server(ioService);
        StreamState state;

        server.setServerErrorCallback([&](const FastNet::Error& error) {
            markFailure(state, error.toString());
        });
        server.setOwnedDataReceivedCallback([&](FastNet::ConnectionId clientId, FastNet::Buffer&& data) {
            std::string requestChunk(reinterpret_cast<const char*>(data.data()), data.size());
            std::string requestText;
            {
                std::lock_guard<std::mutex> lock(state.mutex);
                state.requestText += requestChunk;
                const size_t headerEnd = state.requestText.find("\r\n\r\n");
                if (headerEnd != std::string::npos) {
                    state.requestSeen = true;
                    requestText = state.requestText.substr(0, headerEnd + 4);
                    state.requestText.erase(0, headerEnd + 4);
                }
                state.condition.notify_all();
            }

            if (requestText.empty()) {
                return;
            }

            std::string response;
            if (requestText.find("GET /empty HTTP/1.1") != std::string::npos) {
                response =
                    "HTTP/1.1 204 No Content\r\n"
                    "Content-Length: 0\r\n"
                    "Connection: keep-alive\r\n"
                    "\r\n";
            } else {
                response =
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: text/event-stream\r\n"
                    "Transfer-Encoding: chunked\r\n"
                    "Connection: keep-alive\r\n"
                    "\r\n"
                    "6\r\n"
                    "data: "
                    "\r\n"
                    "5\r\n"
                    "one\n\n"
                    "\r\n"
                    "0\r\n"
                    "\r\n";
            }
            const FastNet::Error sendResult = server.sendToClient(clientId, std::string(response));
            if (sendResult.isFailure()) {
                markFailure(state, sendResult.toString());
            }
        });

        const FastNet::Error startResult = server.start(0, "127.0.0.1");
        FASTNET_TEST_ASSERT_MSG(startResult.isSuccess(), startResult.toString());

        FastNet::HttpClient client(ioService);
        client.setConnectTimeout(3000);
        client.setRequestTimeout(3000);
        client.setReadTimeout(3000);

        const std::string url = "http://127.0.0.1:" + std::to_string(server.getListenAddress().port);
        FASTNET_TEST_ASSERT(client.connect(url, [&](bool success, const std::string& message) {
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
                state.condition.wait_for(lock, 3s, [&]() { return state.connectDone || state.failed; }),
                "Timed out waiting for HTTP streaming client connect");
            FASTNET_TEST_ASSERT_MSG(!state.failed, state.failure);
            FASTNET_TEST_ASSERT(state.connected);
        }

        FastNet::RequestHeaders headers;
        headers["Accept"] = "text/event-stream";
        FASTNET_TEST_ASSERT(client.streamGet(
            "/events",
            headers,
            [&](const FastNet::HttpResponse& response) {
                std::lock_guard<std::mutex> lock(state.mutex);
                state.headersSeen = true;
                state.statusCode = response.statusCode;
                state.condition.notify_all();
            },
            [&](std::string_view chunk) {
                std::lock_guard<std::mutex> lock(state.mutex);
                state.chunks.append(chunk.data(), chunk.size());
                state.condition.notify_all();
                return true;
            },
            [&](const FastNet::HttpResponse& response) {
                std::lock_guard<std::mutex> lock(state.mutex);
                state.complete = true;
                state.statusCode = response.statusCode;
                state.condition.notify_all();
            }));

        {
            std::unique_lock<std::mutex> lock(state.mutex);
            FASTNET_TEST_ASSERT_MSG(
                state.condition.wait_for(lock, 3s, [&]() { return state.complete || state.failed; }),
                "Timed out waiting for HTTP stream completion");
            FASTNET_TEST_ASSERT_MSG(!state.failed, state.failure);
            FASTNET_TEST_ASSERT(state.requestSeen);
            FASTNET_TEST_ASSERT(state.headersSeen);
            FASTNET_TEST_ASSERT_EQ(state.statusCode, 200);
            FASTNET_TEST_ASSERT_EQ(state.chunks, "data: one\n\n");
        }

        {
            std::lock_guard<std::mutex> lock(state.mutex);
            state.headersSeen = false;
            state.complete = false;
            state.requestSeen = false;
            state.statusCode = 0;
            state.chunks.clear();
        }

        FASTNET_TEST_ASSERT(client.streamGet(
            "/empty",
            {},
            [&](const FastNet::HttpResponse& response) {
                std::lock_guard<std::mutex> lock(state.mutex);
                state.headersSeen = true;
                state.statusCode = response.statusCode;
                state.condition.notify_all();
            },
            [&](std::string_view chunk) {
                std::lock_guard<std::mutex> lock(state.mutex);
                state.chunks.append(chunk.data(), chunk.size());
                state.condition.notify_all();
                return true;
            },
            [&](const FastNet::HttpResponse& response) {
                std::lock_guard<std::mutex> lock(state.mutex);
                state.complete = true;
                state.statusCode = response.statusCode;
                state.condition.notify_all();
            }));

        {
            std::unique_lock<std::mutex> lock(state.mutex);
            FASTNET_TEST_ASSERT_MSG(
                state.condition.wait_for(lock, 3s, [&]() { return state.complete || state.failed; }),
                "Timed out waiting for empty HTTP stream completion");
            FASTNET_TEST_ASSERT_MSG(!state.failed, state.failure);
            FASTNET_TEST_ASSERT(state.requestSeen);
            FASTNET_TEST_ASSERT(state.headersSeen);
            FASTNET_TEST_ASSERT_EQ(state.statusCode, 204);
            FASTNET_TEST_ASSERT(state.chunks.empty());
        }

        client.disconnect();
        server.stop();
        std::cout << "http streaming tests passed" << '\n';
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << '\n';
        return 1;
    }
}
