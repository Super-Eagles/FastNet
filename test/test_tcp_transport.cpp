#include "FastNet/FastNet.h"
#include "TestSupport.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <string>

namespace {

struct TcpState {
    std::mutex mutex;
    std::condition_variable condition;
    bool connected = false;
    bool connectFailed = false;
    bool received = false;
    bool disconnected = false;
    std::string failure;
    FastNet::Buffer payload;
    FastNet::ConnectionId serverClientId = 0;
};

void markFailure(TcpState& state, std::string message) {
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.failure.empty()) {
        state.failure = std::move(message);
    }
    state.condition.notify_all();
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
    FastNet::TcpServer server(ioService);
    TcpState state;
    const std::string message(32 * 1024, 'x');

    server.setMaxConnections(8);
    server.setConnectionTimeout(0);
    server.setReadTimeout(2000);
    server.setWriteTimeout(2000);
    server.setServerErrorCallback([&](const FastNet::Error& error) {
        markFailure(state, error.toString());
    });
    server.setClientConnectedCallback([&](FastNet::ConnectionId clientId, const FastNet::Address&) {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.serverClientId = clientId;
        state.condition.notify_all();
    });
    server.setClientDisconnectedCallback([&](FastNet::ConnectionId, const std::string&) {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.disconnected = true;
        state.condition.notify_all();
    });
    server.setOwnedDataReceivedCallback([&](FastNet::ConnectionId clientId, FastNet::Buffer&& data) {
        const FastNet::Error result = server.sendToClient(clientId, std::move(data));
        if (result.isFailure()) {
            markFailure(state, result.toString());
        }
    });

    const FastNet::Error startResult = server.start(0, "127.0.0.1");
    FASTNET_TEST_ASSERT_MSG(startResult.isSuccess(), startResult.toString());
    FASTNET_TEST_ASSERT(server.isRunning());
    FASTNET_TEST_ASSERT(server.getListenAddress().port != 0);

    FastNet::TcpClient client(ioService);
    client.setConnectTimeout(3000);
    client.setReadTimeout(3000);
    client.setWriteTimeout(3000);
    client.setErrorCallback([&](FastNet::ErrorCode, const std::string& messageText) {
        markFailure(state, messageText);
    });
    client.setDataReceivedCallback([&](const FastNet::Buffer& data) {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.payload.insert(state.payload.end(), data.begin(), data.end());
        if (state.payload.size() >= message.size()) {
            state.received = true;
        }
        state.condition.notify_all();
    });
    client.setDisconnectCallback([&](const std::string&) {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.disconnected = true;
        state.condition.notify_all();
    });

    FASTNET_TEST_ASSERT(client.connect(server.getListenAddress(), [&](bool success, const std::string& errorMessage) {
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            state.connected = success;
            state.connectFailed = !success;
            if (!success) {
                state.failure = errorMessage;
            }
            state.condition.notify_all();
        }
        if (success && !client.send(std::string_view(message))) {
            markFailure(state, "client failed to send TCP payload");
        }
    }));

    {
        std::unique_lock<std::mutex> lock(state.mutex);
        FASTNET_TEST_ASSERT_MSG(
            state.condition.wait_for(lock, 5s, [&]() { return state.received || state.connectFailed || !state.failure.empty(); }),
            "Timed out waiting for TCP echo");
        FASTNET_TEST_ASSERT_MSG(state.failure.empty(), state.failure);
        FASTNET_TEST_ASSERT(state.connected);
        FASTNET_TEST_ASSERT(state.received);
        FASTNET_TEST_ASSERT_EQ(state.payload.size(), message.size());
        FASTNET_TEST_ASSERT(std::equal(state.payload.begin(), state.payload.end(), message.begin()));
        FASTNET_TEST_ASSERT(state.serverClientId != 0);
    }

    FASTNET_TEST_ASSERT_EQ(server.getClientCount(), static_cast<size_t>(1));
    FASTNET_TEST_ASSERT(server.hasClient(state.serverClientId));
    FASTNET_TEST_ASSERT(server.getClientAddress(state.serverClientId).port != 0);
    FASTNET_TEST_ASSERT(!server.getClientIds().empty());

    FastNet::TcpConnectionPoolOptions options;
    options.minConnections = 1;
    options.maxConnections = 2;
    options.connectionTimeout = 3000;
    options.acquireTimeout = 3000;
    options.idleTimeout = 5000;
    options.checkInterval = 50;
    FastNet::TcpConnectionPool pool(ioService, "127.0.0.1", server.getListenAddress().port, options);
    FASTNET_TEST_ASSERT(pool.initialize().isSuccess());
    std::shared_ptr<FastNet::PooledConnection> pooled;
    FASTNET_TEST_ASSERT_MSG(pool.acquireSync(pooled).isSuccess(), "pool acquireSync should succeed");
    FASTNET_TEST_ASSERT(pooled != nullptr);
    FASTNET_TEST_ASSERT(pooled->isValid());
    FASTNET_TEST_ASSERT(pooled->getClient() != nullptr);
    FASTNET_TEST_ASSERT_EQ(pooled->getHost(), std::string("127.0.0.1"));
    FASTNET_TEST_ASSERT_EQ(pooled->getPort(), server.getListenAddress().port);
    FASTNET_TEST_ASSERT_EQ(pooled->getState(), FastNet::PooledConnectionState::InUse);
    pooled->updateUsedTime();
    FASTNET_TEST_ASSERT(pool.getInUseConnectionCount() >= 1);
    pool.release(pooled);
    FASTNET_TEST_ASSERT(pool.getIdleConnectionCount() >= 1);
    pool.shutdown();

    client.disconnectAfterPendingWrites();
    server.disconnectClient(state.serverClientId);
    server.stop();
    FASTNET_TEST_ASSERT(!server.isRunning());

    std::cout << "tcp transport tests passed" << '\n';
    return 0;
}
