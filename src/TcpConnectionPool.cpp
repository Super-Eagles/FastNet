/**
 * @file TcpConnectionPool.cpp
 * @brief FastNet TCP connection pool implementation
 */
#include "TcpConnectionPool.h"

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <utility>

namespace FastNet {

PooledConnection::PooledConnection(std::unique_ptr<TcpClient> client, std::string host, uint16_t port)
    : client_(std::move(client)),
      host_(std::move(host)),
      port_(port),
      state_(PooledConnectionState::Idle),
      lastUsedTime_(std::chrono::steady_clock::now()) {}

PooledConnection::~PooledConnection() {
    close();
}

TcpClient* PooledConnection::getClient() const {
    return client_.get();
}

const std::string& PooledConnection::getHost() const {
    return host_;
}

uint16_t PooledConnection::getPort() const {
    return port_;
}

bool PooledConnection::isValid() const {
    return client_ != nullptr &&
           client_->isConnected() &&
           getState() != PooledConnectionState::Closing &&
           getState() != PooledConnectionState::Closed &&
           getState() != PooledConnectionState::Error;
}

std::chrono::steady_clock::time_point PooledConnection::getLastUsedTime() const {
    return lastUsedTime_;
}

void PooledConnection::updateUsedTime() {
    lastUsedTime_ = std::chrono::steady_clock::now();
}

PooledConnectionState PooledConnection::getState() const {
    return state_.load(std::memory_order_acquire);
}

void PooledConnection::setState(PooledConnectionState state) {
    state_.store(state, std::memory_order_release);
}

void PooledConnection::close() {
    auto state = getState();
    if (state == PooledConnectionState::Closed) {
        return;
    }

    setState(PooledConnectionState::Closing);
    if (client_) {
        client_->disconnect();
    }
    setState(PooledConnectionState::Closed);
}

TcpConnectionPool::TcpConnectionPool(IoService& ioService,
                                     const std::string& host,
                                     uint16_t port,
                                     const TcpConnectionPoolOptions& options)
    : options_(options),
      host_(host),
      port_(port),
      ioService_(ioService) {}

TcpConnectionPool::~TcpConnectionPool() {
    shutdown();
}

Error TcpConnectionPool::initialize() {
    bool expected = false;
    if (!initialized_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return FASTNET_ERROR(ErrorCode::AlreadyRunning, "TCP connection pool already initialized");
    }

    shutdown_.store(false, std::memory_order_release);

    for (size_t i = 0; i < options_.minConnections; ++i) {
        auto connection = createConnection();
        if (!connection) {
            break;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        connection->setState(PooledConnectionState::Idle);
        idleConnections_.push_back(connection);
        idleConnectionsCount_.fetch_add(1, std::memory_order_relaxed);
    }

    maintenanceThread_ = std::thread(&TcpConnectionPool::maintenanceLoop, this);
    return FASTNET_SUCCESS;
}

void TcpConnectionPool::acquire(const AcquireCallback& callback) {
    if (!callback) {
        return;
    }

    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (shutdown_.load(std::memory_order_acquire)) {
            lock.unlock();
            ioService_.post([callback]() {
                callback(FASTNET_ERROR(ErrorCode::InvalidArgument, "TCP connection pool is shut down"), nullptr);
            });
            return;
        }

        auto connection = tryAcquireIdleLocked();
        if (connection) {
            connection->setState(PooledConnectionState::InUse);
            connection->updateUsedTime();
            inUseConnectionsCount_.fetch_add(1, std::memory_order_relaxed);
            lock.unlock();
            ioService_.post([callback, connection]() {
                callback(FASTNET_SUCCESS, connection);
            });
            return;
        }

        const size_t totalInFlight =
            static_cast<size_t>(currentConnections_.load(std::memory_order_relaxed)) +
            creatingConnections_.load(std::memory_order_relaxed);
        if (totalInFlight >= options_.maxConnections) {
            pendingAcquires_.push_back(PendingAcquire{callback});
            return;
        }
    }

    std::shared_ptr<PooledConnection> connection;
    const Error result = acquireSync(connection);
    ioService_.post([callback, result, connection]() {
        callback(result, connection);
    });
}

Error TcpConnectionPool::acquireSync(std::shared_ptr<PooledConnection>& connection) {
    if (shutdown_.load(std::memory_order_acquire)) {
        return FASTNET_ERROR(ErrorCode::InvalidArgument, "TCP connection pool is shut down");
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(options_.acquireTimeout);

    while (!shutdown_.load(std::memory_order_acquire)) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            connection = tryAcquireIdleLocked();
            if (connection) {
                connection->setState(PooledConnectionState::InUse);
                connection->updateUsedTime();
                inUseConnectionsCount_.fetch_add(1, std::memory_order_relaxed);
                return FASTNET_SUCCESS;
            }

            const size_t totalInFlight =
                static_cast<size_t>(currentConnections_.load(std::memory_order_relaxed)) +
                creatingConnections_.load(std::memory_order_relaxed);
            if (totalInFlight < options_.maxConnections) {
                creatingConnections_.fetch_add(1, std::memory_order_relaxed);
                lock.unlock();

                auto created = createConnection();

                lock.lock();
                creatingConnections_.fetch_sub(1, std::memory_order_relaxed);
                acquireCondition_.notify_all();

                if (created) {
                    created->setState(PooledConnectionState::InUse);
                    created->updateUsedTime();
                    connection = std::move(created);
                    inUseConnectionsCount_.fetch_add(1, std::memory_order_relaxed);
                    return FASTNET_SUCCESS;
                }
            }

            if (options_.acquireTimeout == 0) {
                break;
            }

            if (acquireCondition_.wait_until(lock, deadline, [this]() {
                    return shutdown_.load(std::memory_order_acquire) ||
                           !idleConnections_.empty() ||
                           currentConnections_.load(std::memory_order_relaxed) +
                                   creatingConnections_.load(std::memory_order_relaxed) <
                               options_.maxConnections;
                })) {
                continue;
            }
        }

        if (std::chrono::steady_clock::now() >= deadline) {
            break;
        }
    }

    return FASTNET_ERROR(ErrorCode::TimeoutError, "Timed out waiting for a pooled TCP connection");
}

void TcpConnectionPool::release(std::shared_ptr<PooledConnection> connection) {
    if (!connection) {
        return;
    }

    const bool reusable = canReuse(connection);
    AcquireCallback pendingCallback;
    bool shouldClose = false;
    const bool wasInUse = connection->getState() == PooledConnectionState::InUse;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (shutdown_.load(std::memory_order_acquire)) {
            shouldClose = true;
        } else if (reusable && !pendingAcquires_.empty()) {
            pendingCallback = std::move(pendingAcquires_.front().callback);
            pendingAcquires_.pop_front();
            connection->setState(PooledConnectionState::InUse);
            connection->updateUsedTime();
        } else if (reusable) {
            connection->setState(PooledConnectionState::Idle);
            connection->updateUsedTime();
            idleConnections_.push_back(connection);
            idleConnectionsCount_.fetch_add(1, std::memory_order_relaxed);
        } else {
            shouldClose = true;
        }
    }

    if (wasInUse && !pendingCallback) {
        inUseConnectionsCount_.fetch_sub(1, std::memory_order_relaxed);
    }

    if (pendingCallback) {
        ioService_.post([callback = std::move(pendingCallback), connection]() {
            callback(FASTNET_SUCCESS, connection);
        });
        return;
    }

    if (shouldClose) {
        connection->setState(PooledConnectionState::Error);
        connection->close();

        std::lock_guard<std::mutex> lock(mutex_);
        auto it = std::remove(connections_.begin(), connections_.end(), connection);
        if (it != connections_.end()) {
            connections_.erase(it, connections_.end());
        }
        currentConnections_.fetch_sub(1, std::memory_order_relaxed);
        acquireCondition_.notify_all();
    } else {
        acquireCondition_.notify_one();
    }
}

void TcpConnectionPool::shutdown() {
    if (shutdown_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    acquireCondition_.notify_all();
    if (maintenanceThread_.joinable()) {
        maintenanceThread_.join();
    }

    std::vector<std::shared_ptr<PooledConnection>> connections;
    std::deque<PendingAcquire> pendingAcquires;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        connections.swap(connections_);
        pendingAcquires.swap(pendingAcquires_);
        idleConnections_.clear();
    }

    for (auto& connection : connections) {
        if (connection) {
            connection->close();
        }
    }

    for (auto& pending : pendingAcquires) {
        if (!pending.callback) {
            continue;
        }
        ioService_.post([callback = std::move(pending.callback)]() {
            callback(FASTNET_ERROR(ErrorCode::InvalidArgument, "TCP connection pool is shut down"), nullptr);
        });
    }

    currentConnections_.store(0, std::memory_order_release);
    idleConnectionsCount_.store(0, std::memory_order_release);
    inUseConnectionsCount_.store(0, std::memory_order_release);
    creatingConnections_.store(0, std::memory_order_release);
    initialized_.store(false, std::memory_order_release);
}

uint32_t TcpConnectionPool::getCurrentConnectionCount() const {
    return currentConnections_.load(std::memory_order_acquire);
}

uint32_t TcpConnectionPool::getIdleConnectionCount() const {
    return idleConnectionsCount_.load(std::memory_order_acquire);
}

uint32_t TcpConnectionPool::getInUseConnectionCount() const {
    return inUseConnectionsCount_.load(std::memory_order_acquire);
}

std::shared_ptr<PooledConnection> TcpConnectionPool::createConnection() {
    auto client = std::make_unique<TcpClient>(ioService_);
    client->setConnectTimeout(options_.connectionTimeout);

    std::mutex connectMutex;
    std::condition_variable connectCondition;
    bool finished = false;
    bool connected = false;
    std::string errorMessage;

    if (!client->connect(host_, port_,
                         [&](bool success, const std::string& message) {
                             std::lock_guard<std::mutex> lock(connectMutex);
                             connected = success;
                             errorMessage = message;
                             finished = true;
                             connectCondition.notify_one();
                         },
                         options_.sslConfig)) {
        return nullptr;
    }

    {
        std::unique_lock<std::mutex> lock(connectMutex);
        connectCondition.wait_for(lock,
                                  std::chrono::milliseconds(options_.connectionTimeout),
                                  [&finished]() { return finished; });
    }

    if (!finished || !connected) {
        (void)errorMessage;
        client->disconnect();
        return nullptr;
    }

    auto connection = std::make_shared<PooledConnection>(std::move(client), host_, port_);
    connection->setState(PooledConnectionState::Idle);

    std::lock_guard<std::mutex> lock(mutex_);
    connections_.push_back(connection);
    currentConnections_.fetch_add(1, std::memory_order_relaxed);
    return connection;
}

std::shared_ptr<PooledConnection> TcpConnectionPool::tryAcquireIdleLocked() {
    while (!idleConnections_.empty()) {
        auto connection = idleConnections_.front();
        idleConnections_.pop_front();
        idleConnectionsCount_.fetch_sub(1, std::memory_order_relaxed);

        if (canReuse(connection)) {
            return connection;
        }

        connection->setState(PooledConnectionState::Error);
        connection->close();
        auto it = std::remove(connections_.begin(), connections_.end(), connection);
        if (it != connections_.end()) {
            connections_.erase(it, connections_.end());
        }
        currentConnections_.fetch_sub(1, std::memory_order_relaxed);
    }

    return nullptr;
}

bool TcpConnectionPool::canReuse(const std::shared_ptr<PooledConnection>& connection) const {
    if (!connection || !connection->isValid()) {
        return false;
    }

    const auto idleFor = std::chrono::steady_clock::now() - connection->getLastUsedTime();
    return idleFor <= std::chrono::milliseconds(options_.idleTimeout);
}

void TcpConnectionPool::fulfillPendingAcquire(std::shared_ptr<PooledConnection> connection) {
    AcquireCallback callback;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pendingAcquires_.empty()) {
            idleConnections_.push_back(connection);
            idleConnectionsCount_.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        callback = std::move(pendingAcquires_.front().callback);
        pendingAcquires_.pop_front();
    }

    ioService_.post([callback = std::move(callback), connection]() {
        callback(FASTNET_SUCCESS, connection);
    });
}

void TcpConnectionPool::maintenanceLoop() {
    while (!shutdown_.load(std::memory_order_acquire)) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (acquireCondition_.wait_for(
                lock,
                std::chrono::milliseconds(options_.checkInterval),
                [this]() { return shutdown_.load(std::memory_order_acquire); })) {
            break;
        }
        lock.unlock();

        cleanupIdleConnections();
        ensureMinimumConnections();
    }
}

void TcpConnectionPool::cleanupIdleConnections() {
    std::vector<std::shared_ptr<PooledConnection>> staleConnections;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::deque<std::shared_ptr<PooledConnection>> retained;
        const auto now = std::chrono::steady_clock::now();

        while (!idleConnections_.empty()) {
            auto connection = idleConnections_.front();
            idleConnections_.pop_front();

            const bool belowMinimum = retained.size() + staleConnections.size() + 1 <= options_.minConnections;
            const bool expired =
                now - connection->getLastUsedTime() > std::chrono::milliseconds(options_.idleTimeout);

            if (!belowMinimum && (!canReuse(connection) || expired)) {
                idleConnectionsCount_.fetch_sub(1, std::memory_order_relaxed);
                staleConnections.push_back(connection);
                auto it = std::remove(connections_.begin(), connections_.end(), connection);
                if (it != connections_.end()) {
                    connections_.erase(it, connections_.end());
                }
                currentConnections_.fetch_sub(1, std::memory_order_relaxed);
                continue;
            }

            retained.push_back(connection);
        }

        idleConnections_.swap(retained);
    }

    for (auto& connection : staleConnections) {
        connection->close();
    }

    acquireCondition_.notify_all();
}

void TcpConnectionPool::ensureMinimumConnections() {
    while (!shutdown_.load(std::memory_order_acquire) &&
           currentConnections_.load(std::memory_order_relaxed) +
                   creatingConnections_.load(std::memory_order_relaxed) <
               options_.minConnections) {
        creatingConnections_.fetch_add(1, std::memory_order_relaxed);
        auto connection = createConnection();
        creatingConnections_.fetch_sub(1, std::memory_order_relaxed);

        if (!connection) {
            break;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        connection->setState(PooledConnectionState::Idle);
        idleConnections_.push_back(connection);
        idleConnectionsCount_.fetch_add(1, std::memory_order_relaxed);
        acquireCondition_.notify_all();
    }
}

} // namespace FastNet
