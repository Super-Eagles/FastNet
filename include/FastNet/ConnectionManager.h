/**
 * @file ConnectionManager.h
 * @brief FastNet service-level connection manager
 */
#pragma once

#include "Config.h"
#include "Configuration.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace FastNet {

struct ConnectionInfo {
    ConnectionId id = 0;
    std::string service;
    std::string remoteHost;
    uint16_t remotePort = 0;
    std::chrono::steady_clock::time_point createdAt{};
    std::chrono::steady_clock::time_point lastUsed{};
    bool isActive = false;
};

struct ConnectionPoolStats {
    size_t activeConnections = 0;
    size_t idleConnections = 0;
    size_t totalConnections = 0;
    size_t maxConnections = 0;
    size_t pendingRequests = 0;
};

enum class LoadBalancingStrategy {
    RoundRobin,
    Random,
    LeastConnections,
    WeightedRoundRobin,
    IPHash
};

struct BackendServer {
    std::string host;
    uint16_t port = 0;
    int weight = 1;
    bool healthy = true;
    size_t activeConnections = 0;
    size_t failureCount = 0;
    int currentWeight = 0;
    std::chrono::steady_clock::time_point lastCheckTime{};
    std::chrono::steady_clock::time_point lastFailureTime{};
};

enum class CircuitBreakerState {
    Closed,
    Open,
    HalfOpen
};

struct CircuitBreakerStats {
    CircuitBreakerState state = CircuitBreakerState::Closed;
    size_t failureCount = 0;
    size_t successCount = 0;
    size_t timeoutCount = 0;
    std::chrono::steady_clock::time_point lastFailureTime{};
};

struct ServicePoolStats {
    std::string service;
    size_t activeConnections = 0;
    size_t idleConnections = 0;
    size_t totalConnections = 0;
    size_t maxConnections = 0;
    size_t pendingRequests = 0;
    size_t backendCount = 0;
    size_t healthyBackends = 0;
    LoadBalancingStrategy strategy = LoadBalancingStrategy::RoundRobin;
    CircuitBreakerStats circuitBreaker;
};

class FASTNET_API ConnectionManager {
public:
    explicit ConnectionManager(const Configuration& config);
    ~ConnectionManager();

    bool initialize();
    void cleanup();

    ConnectionId acquireConnection(const std::string& service);
    ConnectionId acquireConnection(const std::string& service, const std::string& affinityKey);
    void releaseConnection(ConnectionId id);
    void closeConnection(ConnectionId id);

    ConnectionPoolStats getPoolStats() const;
    ServicePoolStats getServiceStats(const std::string& service) const;
    std::vector<std::string> getServices() const;
    std::vector<BackendServer> getBackendServers(const std::string& service) const;

    void addBackendServer(const std::string& service,
                          const std::string& host,
                          uint16_t port,
                          int weight = 1);
    void removeBackendServer(const std::string& service, const std::string& host, uint16_t port);
    void updateBackendWeight(const std::string& service,
                             const std::string& host,
                             uint16_t port,
                             int weight);

    LoadBalancingStrategy getLoadBalancingStrategy(const std::string& service) const;
    void setLoadBalancingStrategy(const std::string& service, LoadBalancingStrategy strategy);

    CircuitBreakerStats getCircuitBreakerStats(const std::string& service) const;
    void reportBackendStatus(const std::string& host, uint16_t port, bool success);
    void reportBackendStatus(const std::string& service,
                             const std::string& host,
                             uint16_t port,
                             bool success);
    void reportExecution(bool success);
    void reportExecution(const std::string& service, bool success);

private:
    struct ServiceState {
        std::vector<BackendServer> backends;
        std::deque<ConnectionId> idleConnections;
        LoadBalancingStrategy strategy = LoadBalancingStrategy::RoundRobin;
        CircuitBreakerStats circuitBreaker;
        size_t roundRobinIndex   = 0;
        size_t maxPoolSize       = 0;
        size_t pendingRequests   = 0;
        // O(1) connection count — incremented in createConnectionLocked,
        // decremented in removeConnectionLocked.  Replaces the O(N) scan
        // of connections_ that getServiceConnectionCountLocked() used to do.
        size_t totalConnections  = 0;
    };


    ServiceState& ensureServiceStateLocked(const std::string& service);
    ConnectionId createConnectionLocked(const std::string& service,
                                        BackendServer& backend,
                                        std::chrono::steady_clock::time_point now);
    std::unordered_map<ConnectionId, ConnectionInfo>::iterator removeConnectionLocked(
        std::unordered_map<ConnectionId, ConnectionInfo>::iterator it);
    void cleanupExpiredConnectionsLocked(std::chrono::steady_clock::time_point now);
    void performHealthCheckLocked(std::chrono::steady_clock::time_point now);
    void pruneIdleConnectionsLocked(ServiceState& serviceState);
    BackendServer* selectBackendLocked(ServiceState& serviceState, std::string_view affinityKey);
    bool prepareCircuitBreakerLocked(ServiceState& serviceState, std::chrono::steady_clock::time_point now);
    size_t getServiceConnectionCountLocked(std::string_view service) const;
    static size_t getActiveConnectionCountLocked(const ServiceState& serviceState);
    static LoadBalancingStrategy parseStrategy(std::string_view strategyName);
    void maintenanceLoop();

    Configuration config_;
    mutable std::mutex mutex_;
    std::unordered_map<ConnectionId, ConnectionInfo> connections_;
    std::unordered_map<std::string, ServiceState> services_;
    std::atomic<ConnectionId> nextConnectionId_{1};
    std::atomic<bool> running_{false};
    std::thread maintenanceThread_;
    std::condition_variable maintenanceCondition_;
    size_t pendingRequests_ = 0;
    std::chrono::milliseconds idleTimeout_;
    std::chrono::milliseconds healthCheckInterval_;
    std::chrono::milliseconds recoveryTimeout_;
    bool circuitBreakerEnabled_ = true;
    size_t failureThreshold_ = 5;
    size_t halfOpenAttempts_ = 3;
};

} // namespace FastNet
