#include "FastNet/FastNet.h"
#include "TestSupport.h"

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>

namespace {

struct HttpTestState {
    std::mutex mutex;
    std::condition_variable condition;
    bool connected = false;
    bool connectDone = false;
    bool failed = false;
    std::string failure;
};

struct ResponseWaiter {
    std::mutex mutex;
    std::condition_variable condition;
    bool done = false;
    FastNet::HttpResponse response;
};

void markFailure(HttpTestState& state, std::string message) {
    std::lock_guard<std::mutex> lock(state.mutex);
    if (!state.failed) {
        state.failed = true;
        state.failure = std::move(message);
    }
    state.condition.notify_all();
}

FastNet::HttpResponse request(FastNet::HttpClient& client,
                              std::string_view method,
                              std::string_view path,
                              std::string_view body = {}) {
    using namespace std::chrono_literals;
    ResponseWaiter waiter;
    FastNet::RequestHeaders headers;
    if (!body.empty()) {
        headers["Content-Type"] = "text/plain";
    }
    const bool issued = client.request(method, path, headers, body, [&](const FastNet::HttpResponse& response) {
        std::lock_guard<std::mutex> lock(waiter.mutex);
        waiter.response = response;
        waiter.done = true;
        waiter.condition.notify_all();
    });
    FASTNET_TEST_ASSERT_MSG(issued, client.getLastError().toString());

    std::unique_lock<std::mutex> lock(waiter.mutex);
    FASTNET_TEST_ASSERT_MSG(
        waiter.condition.wait_for(lock, 3s, [&]() { return waiter.done; }),
        "Timed out waiting for HTTP response");
    return waiter.response;
}

} // namespace

int main() {
    using namespace std::chrono_literals;

    FASTNET_TEST_ASSERT_EQ(FastNet::initialize(2), FastNet::ErrorCode::Success);
    struct CleanupGuard {
        ~CleanupGuard() {
            FastNet::cleanup();
        }
    } cleanupGuard;

    auto& ioService = FastNet::getGlobalIoService();
    FastNet::HttpServer server(ioService);
    HttpTestState state;

    server.setMaxConnections(16);
    server.setConnectionTimeout(0);
    server.setRequestTimeout(3000);
    server.setWriteTimeout(3000);
    server.setMaxRequestSize(1024 * 1024);
    server.setServerErrorCallback([&](const FastNet::Error& error) {
        markFailure(state, error.toString());
    });

    server.registerGet("/method", [](const FastNet::HttpRequest& request, FastNet::HttpResponse& response) {
        response.body = "GET:" + request.queryString;
    });
    server.registerPost("/method", [](const FastNet::HttpRequest& request, FastNet::HttpResponse& response) {
        response.body = "POST:" + request.body;
    });
    server.registerPut("/method", [](const FastNet::HttpRequest& request, FastNet::HttpResponse& response) {
        response.body = "PUT:" + request.body;
    });
    server.registerPatch("/method", [](const FastNet::HttpRequest& request, FastNet::HttpResponse& response) {
        response.body = "PATCH:" + request.body;
    });
    server.registerDelete("/method", [](const FastNet::HttpRequest&, FastNet::HttpResponse& response) {
        response.statusCode = 204;
        response.statusMessage = "No Content";
    });
    server.registerHead("/method", [](const FastNet::HttpRequest&, FastNet::HttpResponse& response) {
        response.body = "head-body-hidden";
    });
    server.registerOptions("/method", [](const FastNet::HttpRequest&, FastNet::HttpResponse& response) {
        response.headers["Allow"] = "GET,POST,PUT,PATCH,DELETE,HEAD,OPTIONS";
        response.body = "OPTIONS";
    });
    server.setRequestHandler([](const FastNet::HttpRequest& request, FastNet::HttpResponse& response) {
        response.statusCode = 202;
        response.statusMessage = "Accepted";
        response.body = "DEFAULT:" + request.path;
    });

    std::filesystem::create_directories("tmp/http-static");
    {
        std::ofstream file("tmp/http-static/hello.txt", std::ios::binary | std::ios::trunc);
        file << "static-body";
    }
    server.setStaticFileCacheLimit(1024 * 1024);
    server.registerStaticFileHandler("/static", "tmp/http-static");

    const FastNet::Error startResult = server.start(0, "127.0.0.1");
    FASTNET_TEST_ASSERT_MSG(startResult.isSuccess(), startResult.toString());
    FASTNET_TEST_ASSERT(server.isRunning());
    FASTNET_TEST_ASSERT(server.getListenAddress().port != 0);

    FastNet::HttpClient client(ioService);
    client.setConnectTimeout(3000);
    client.setRequestTimeout(3000);
    client.setReadTimeout(3000);
    client.setFollowRedirects(true);
    client.setMaxRedirects(2);
    client.setUseCompression(false);

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
            "Timed out waiting for HTTP client connect");
        FASTNET_TEST_ASSERT_MSG(!state.failed, state.failure);
        FASTNET_TEST_ASSERT(state.connected);
    }
    FASTNET_TEST_ASSERT(client.isConnected());
    FASTNET_TEST_ASSERT(client.getRemoteAddress().port == server.getListenAddress().port);

    auto response = request(client, "GET", "/method?x=1");
    FASTNET_TEST_ASSERT_EQ(response.statusCode, 200);
    FASTNET_TEST_ASSERT_EQ(response.body, "GET:x=1");

    response = request(client, "POST", "/method", "post-body");
    FASTNET_TEST_ASSERT_EQ(response.statusCode, 200);
    FASTNET_TEST_ASSERT_EQ(response.body, "POST:post-body");

    response = request(client, "PUT", "/method", "put-body");
    FASTNET_TEST_ASSERT_EQ(response.body, "PUT:put-body");

    response = request(client, "PATCH", "/method", "patch-body");
    FASTNET_TEST_ASSERT_EQ(response.body, "PATCH:patch-body");

    response = request(client, "DELETE", "/method");
    FASTNET_TEST_ASSERT_EQ(response.statusCode, 204);

    response = request(client, "HEAD", "/method");
    FASTNET_TEST_ASSERT_EQ(response.statusCode, 200);
    FASTNET_TEST_ASSERT(response.body.empty());

    response = request(client, "OPTIONS", "/method");
    FASTNET_TEST_ASSERT_EQ(response.statusCode, 200);
    FASTNET_TEST_ASSERT(response.headers["Allow"].find("GET") != std::string::npos);

    response = request(client, "GET", "/static/hello.txt");
    FASTNET_TEST_ASSERT_EQ(response.statusCode, 200);
    FASTNET_TEST_ASSERT_EQ(response.body, "static-body");

    response = request(client, "GET", "/fallback");
    FASTNET_TEST_ASSERT_EQ(response.statusCode, 202);
    FASTNET_TEST_ASSERT_EQ(response.body, "DEFAULT:/fallback");

    FASTNET_TEST_ASSERT(!server.getClientIds().empty());
    FASTNET_TEST_ASSERT(server.getClientCount() >= 1);
    client.disconnect();
    server.stop();
    FASTNET_TEST_ASSERT(!server.isRunning());

    std::cout << "http loopback tests passed" << '\n';
    return 0;
}
