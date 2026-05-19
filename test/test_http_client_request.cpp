#include "FastNet/FastNet.h"
#include "TestSupport.h"

#include <chrono>
#include <condition_variable>
#include <exception>
#include <iostream>
#include <mutex>
#include <string>

namespace {

struct RequestApiState {
    std::mutex mutex;
    std::condition_variable condition;
    bool connectDone = false;
    bool connected = false;
    bool proxyRequestSeen = false;
    bool responseDone = false;
    bool failed = false;
    std::string proxyRequest;
    std::string failure;
    FastNet::HttpResponse response;
};

void markFailure(RequestApiState& state, std::string message) {
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
        FastNet::TcpServer proxy(ioService);
        RequestApiState state;

        proxy.setServerErrorCallback([&](const FastNet::Error& error) {
            markFailure(state, error.toString());
        });
        proxy.setOwnedDataReceivedCallback([&](FastNet::ConnectionId clientId, FastNet::Buffer&& data) {
            std::string chunk(reinterpret_cast<const char*>(data.data()), data.size());
            bool shouldReply = false;
            {
                std::lock_guard<std::mutex> lock(state.mutex);
                state.proxyRequest += chunk;
                shouldReply = !state.proxyRequestSeen && state.proxyRequest.find("\r\n\r\n") != std::string::npos;
                if (shouldReply) {
                    state.proxyRequestSeen = true;
                }
                state.condition.notify_all();
            }
            if (!shouldReply) {
                return;
            }

            const std::string response =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/plain\r\n"
                "Content-Length: 8\r\n"
                "Connection: keep-alive\r\n"
                "\r\n"
                "proxied!";
            const FastNet::Error sendResult = proxy.sendToClient(clientId, std::string(response));
            if (sendResult.isFailure()) {
                markFailure(state, sendResult.toString());
            }
        });

        const FastNet::Error startResult = proxy.start(0, "127.0.0.1");
        FASTNET_TEST_ASSERT_MSG(startResult.isSuccess(), startResult.toString());

        FastNet::HttpClient client(ioService);
        FASTNET_TEST_ASSERT(client.setProxyUrl("http://proxy-user:proxy-pass@127.0.0.1:" +
                                               std::to_string(proxy.getListenAddress().port)));
        client.setConnectTimeout(3000);
        client.setRequestTimeout(3000);
        client.setReadTimeout(3000);

        FASTNET_TEST_ASSERT(client.connect("http://origin.example", [&](bool success, const std::string& message) {
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
                "Timed out waiting for HTTP proxy client connect");
            FASTNET_TEST_ASSERT_MSG(!state.failed, state.failure);
            FASTNET_TEST_ASSERT(state.connected);
        }

        FastNet::HttpClientRequest request("/api/chat");
        request.setMethod("POST")
            .addQuery("q", "hello world")
            .setJson("{\"ok\":true}")
            .setBearerToken("model-token")
            .setUserAgent("FastNet-Test")
            .setAccept("application/json")
            .setTimeout(3s);

        FASTNET_TEST_ASSERT(client.request(request, [&](const FastNet::HttpResponse& response) {
            std::lock_guard<std::mutex> lock(state.mutex);
            state.response = response;
            state.responseDone = true;
            state.condition.notify_all();
        }));

        {
            std::unique_lock<std::mutex> lock(state.mutex);
            FASTNET_TEST_ASSERT_MSG(
                state.condition.wait_for(lock, 3s, [&]() { return state.responseDone || state.failed; }),
                "Timed out waiting for HTTP proxy response");
            FASTNET_TEST_ASSERT_MSG(!state.failed, state.failure);
            FASTNET_TEST_ASSERT(state.proxyRequestSeen);
            FASTNET_TEST_ASSERT_EQ(state.response.statusCode, 200);
            FASTNET_TEST_ASSERT_EQ(state.response.body, "proxied!");
            FASTNET_TEST_ASSERT(
                state.proxyRequest.find("POST http://origin.example/api/chat?q=hello%20world HTTP/1.1") !=
                std::string::npos);
            FASTNET_TEST_ASSERT(state.proxyRequest.find("Host: origin.example\r\n") != std::string::npos);
            FASTNET_TEST_ASSERT(state.proxyRequest.find("Authorization: Bearer model-token\r\n") != std::string::npos);
            FASTNET_TEST_ASSERT(
                state.proxyRequest.find("Proxy-Authorization: Basic cHJveHktdXNlcjpwcm94eS1wYXNz\r\n") !=
                std::string::npos);
            FASTNET_TEST_ASSERT(state.proxyRequest.find("Content-Type: application/json\r\n") != std::string::npos);
            FASTNET_TEST_ASSERT(state.proxyRequest.find("\r\n\r\n{\"ok\":true}") != std::string::npos);
        }

        {
            std::lock_guard<std::mutex> lock(state.mutex);
            state.proxyRequest.clear();
            state.proxyRequestSeen = false;
            state.responseDone = false;
            state.response = {};
        }

        FastNet::RequestHeaders transferEncodingHeaders;
        transferEncodingHeaders["Transfer-Encoding"] = "chunked";
        FASTNET_TEST_ASSERT(client.request("POST",
                                           "/stream",
                                           transferEncodingHeaders,
                                           "3\r\nabc\r\n0\r\n\r\n",
                                           [&](const FastNet::HttpResponse& response) {
                                               std::lock_guard<std::mutex> lock(state.mutex);
                                               state.response = response;
                                               state.responseDone = true;
                                               state.condition.notify_all();
                                           }));

        {
            std::unique_lock<std::mutex> lock(state.mutex);
            FASTNET_TEST_ASSERT_MSG(
                state.condition.wait_for(lock, 3s, [&]() { return state.responseDone || state.failed; }),
                "Timed out waiting for chunked HTTP proxy response");
            FASTNET_TEST_ASSERT_MSG(!state.failed, state.failure);
            FASTNET_TEST_ASSERT_EQ(state.response.statusCode, 200);
            FASTNET_TEST_ASSERT(state.proxyRequestSeen);
            FASTNET_TEST_ASSERT(state.proxyRequest.find("Transfer-Encoding: chunked\r\n") != std::string::npos);
            FASTNET_TEST_ASSERT(state.proxyRequest.find("Content-Length:") == std::string::npos);
            FASTNET_TEST_ASSERT(state.proxyRequest.find("\r\n\r\n3\r\nabc\r\n0\r\n\r\n") != std::string::npos);
        }

        client.disconnect();
        proxy.stop();
        std::cout << "http client request/proxy tests passed" << '\n';
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << '\n';
        return 1;
    }
}
