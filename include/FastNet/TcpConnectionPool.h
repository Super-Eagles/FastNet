/**
 * @file TcpConnectionPool.h
 * @brief FastNet TCP connection pool
 */
#pragma once

#include "Config.h"
#include "Error.h"
#include "TcpClient.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace FastNet {

struct FASTNET_API TcpConnectionPoolOptions {
    size_t minConnections = 1;
    size_t maxConnections = 32;
    uint32_t connectionTimeout = 5000;
    uint32_t acquireTimeout = 5000;
    uint32_t idleTimeout = 30000;
    uint32_t checkInterval = 1000;
    SSLConfig sslConfig;
};

enum class PooledConnectionState {
    Idle,
    InUse,
    Closing,
    Closed,
    Error
};

class FASTNET_API PooledConnection {
public:
    PooledConnection(std::unique_ptr<TcpClient> client, std::string host, uint16_t port);
    ~PooledConnection();

    TcpClient* getClient() const;
    const std::string& getHost() const;
    uint16_t getPort() const;
    bool isValid() const;
    std::chrono::steady_clock::time_point getLastUsedTime() const;
    void updateUsedTime();
    PooledConnectionState getState() const;
    void setState(PooledConnectionState state);
    void close();

private:
    std::unique_ptr<TcpClient> client_;
    std::string host_;
    uint16_t port_ = 0;
    std::atomic<PooledConnectionState> state_{PooledConnectionState::Closed};
    std::chrono::steady_clock::time_point lastUsedTime_{std::chrono::steady_clock::now()};
};

class FASTNET_API TcpConnectionPool {
public:
    using AcquireCallback = std::function<void(const Error&, std::shared_ptr<PooledConnection>)>;

    TcpConnectionPool(IoService& ioService,
                      const std::string& host,
                      uint16_t port,
                      const TcpConnectionPoolOptions& options = TcpConnectionPoolOptions());
    ~TcpConnectionPool();

    Error initialize();
    void acquire(const AcquireCallback& callback);
    Error acquireSync(std::shared_ptr<PooledConnection>& connection);
    void release(std::shared_ptr<PooledConnection> connection);
    void shutdown();

    uint32_t getCurrentConnectionCount() const;
    uint32_t getIdleConnectionCount() const;
    uint32_t getInUseConnectionCount() const;

private:
    struct PendingAcquire {
        AcquireCallback callback;
    };

    std::shared_ptr<PooledConnection> createConnection();
    std::shared_ptr<PooledConnection> tryAcquireIdleLocked();
    bool canReuse(const std::shared_ptr<PooledConnection>& connection) const;
    void fulfillPendingAcquire(std::shared_ptr<PooledConnection> connection);
    void maintenanceLoop();
    void cleanupIdleConnections();
    void ensureMinimumConnections();

    TcpConnectionPoolOptions options_;
    std::string host_;
    uint16_t port_ = 0;
    IoService& ioService_;

    mutable std::mutex mutex_;
    std::condition_variable acquireCondition_;
    std::vector<std::shared_ptr<PooledConnection>> connections_;
    std::deque<std::shared_ptr<PooledConnection>> idleConnections_;
    std::deque<PendingAcquire> pendingAcquires_;
    std::thread maintenanceThread_;
    std::atomic<uint32_t> currentConnections_{0};
    std::atomic<uint32_t> idleConnectionsCount_{0};
    std::atomic<uint32_t> inUseConnectionsCount_{0};
    std::atomic<size_t> creatingConnections_{0};
    std::atomic<bool> initialized_{false};
    std::atomic<bool> shutdown_{false};
};

} // namespace FastNet
