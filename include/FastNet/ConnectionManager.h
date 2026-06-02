/**
 * @file ConnectionManager.h
 * @brief FastNet service-level connection manager
 */
#pragma once

#include "Config.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace FastNet {

class Configuration;

#pragma warning(push)
#pragma warning(disable: 4251)

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

#pragma warning(pop)

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
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace FastNet
