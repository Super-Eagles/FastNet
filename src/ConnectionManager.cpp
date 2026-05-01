/**
 * @file ConnectionManager.cpp
 * @brief FastNet service-level connection manager
 */
#include "ConnectionManager.h"

#include <algorithm>
#include <cctype>
#include <random>
#include <utility>

namespace FastNet {

namespace {

std::string normalizeStrategyName(std::string_view text) {
    std::string normalized;
    normalized.reserve(text.size());
    for (char ch : text) {
        const unsigned char uch = static_cast<unsigned char>(ch);
        if (std::isalnum(uch)) {
            normalized.push_back(static_cast<char>(std::tolower(uch)));
        }
    }
    return normalized;
}

void applyBackendStatus(BackendServer& backend,
                        bool success,
                        size_t failureThreshold,
                        std::chrono::steady_clock::time_point now) {
    backend.lastCheckTime = now;
    if (success) {
        backend.failureCount = 0;
        backend.healthy = true;
        backend.currentWeight = 0;
        return;
    }

    ++backend.failureCount;
    backend.lastFailureTime = now;
    if (backend.failureCount >= failureThreshold) {
        backend.healthy = false;
        backend.currentWeight = 0;
    }
}

void applyExecutionResult(CircuitBreakerStats& breaker,
                          bool success,
                          size_t failureThreshold,
                          size_t halfOpenAttempts,
                          std::chrono::steady_clock::time_point now) {
    if (success) {
        ++breaker.successCount;
        breaker.timeoutCount = 0;
        if (breaker.state == CircuitBreakerState::HalfOpen &&
            breaker.successCount >= halfOpenAttempts) {
            breaker.state = CircuitBreakerState::Closed;
            breaker.failureCount = 0;
            breaker.successCount = 0;
        } else if (breaker.state == CircuitBreakerState::Closed) {
            breaker.failureCount = 0;
        }
        return;
    }

    ++breaker.failureCount;
    ++breaker.timeoutCount;
    breaker.lastFailureTime = now;
    if (breaker.state == CircuitBreakerState::HalfOpen ||
        breaker.failureCount >= failureThreshold) {
        breaker.state = CircuitBreakerState::Open;
        breaker.successCount = 0;
    }
}

bool backendMatchesConnection(const BackendServer& backend, const ConnectionInfo& connection) {
    return backend.host == connection.remoteHost && backend.port == connection.remotePort;
}

} // namespace

ConnectionManager::ConnectionManager(const Configuration& config)
    : config_(config),
      idleTimeout_(config_.getInt(Configuration::Option::ConnectionTimeout, 30000)),
      healthCheckInterval_(config_.getInt(Configuration::Option::HealthCheckInterval, 30000)),
      recoveryTimeout_(config_.getInt(Configuration::Option::RecoveryTimeout, 60000)),
      circuitBreakerEnabled_(config_.getBool(Configuration::Option::CircuitBreakerEnabled, true)),
      failureThreshold_(static_cast<size_t>(std::max(1, config_.getInt(Configuration::Option::FailureThreshold, 5)))),
      halfOpenAttempts_(static_cast<size_t>(std::max(1, config_.getInt(Configuration::Option::HalfOpenAttempts, 3)))) {}

ConnectionManager::~ConnectionManager() {
    cleanup();
}

bool ConnectionManager::initialize() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return true;
    }

    maintenanceThread_ = std::thread(&ConnectionManager::maintenanceLoop, this);
    return true;
}

void ConnectionManager::cleanup() {
    const bool wasRunning = running_.exchange(false, std::memory_order_acq_rel);
    if (wasRunning) {
        maintenanceCondition_.notify_all();
        if (maintenanceThread_.joinable()) {
            maintenanceThread_.join();
        }
    }

    std::lock_guard<std::mutex> lock(mutex_);
    connections_.clear();
    services_.clear();
    pendingRequests_ = 0;
}

ConnectionId ConnectionManager::acquireConnection(const std::string& service) {
    return acquireConnection(service, "");
}

ConnectionId ConnectionManager::acquireConnection(const std::string& service,
                                                  const std::string& affinityKey) {
    if (service.empty()) {
        return 0;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    ServiceState& serviceState = ensureServiceStateLocked(service);
    ++pendingRequests_;
    ++serviceState.pendingRequests;

    const auto finish = [&](ConnectionId id) {
        if (pendingRequests_ > 0) {
            --pendingRequests_;
        }
        if (serviceState.pendingRequests > 0) {
            --serviceState.pendingRequests;
        }
        return id;
    };

    const auto now = std::chrono::steady_clock::now();
    if (!prepareCircuitBreakerLocked(serviceState, now)) {
        return finish(0);
    }

    while (!serviceState.idleConnections.empty()) {
        const ConnectionId candidateId = serviceState.idleConnections.front();
        serviceState.idleConnections.pop_front();

        const auto connectionIt = connections_.find(candidateId);
        if (connectionIt == connections_.end()) {
            continue;
        }

        if (connectionIt->second.isActive) {
            continue;
        }

        if (idleTimeout_.count() > 0 && now - connectionIt->second.lastUsed > idleTimeout_) {
            removeConnectionLocked(connectionIt);
            continue;
        }

        connectionIt->second.isActive = true;
        connectionIt->second.lastUsed = now;

        auto backendIt = std::find_if(
            serviceState.backends.begin(),
            serviceState.backends.end(),
            [&](const BackendServer& backend) {
                return backendMatchesConnection(backend, connectionIt->second);
            });
        if (backendIt == serviceState.backends.end() || !backendIt->healthy) {
            removeConnectionLocked(connectionIt);
            continue;
        }

        ++backendIt->activeConnections;

        return finish(candidateId);
    }

    if (getServiceConnectionCountLocked(service) >= serviceState.maxPoolSize) {
        return finish(0);
    }

    BackendServer* backend = selectBackendLocked(serviceState, affinityKey);
    if (backend == nullptr) {
        return finish(0);
    }

    return finish(createConnectionLocked(service, *backend, now));
}

void ConnectionManager::releaseConnection(ConnectionId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto connectionIt = connections_.find(id);
    if (connectionIt == connections_.end() || !connectionIt->second.isActive) {
        return;
    }

    auto serviceIt = services_.find(connectionIt->second.service);
    if (serviceIt == services_.end()) {
        removeConnectionLocked(connectionIt);
        return;
    }

    auto backendIt = std::find_if(
        serviceIt->second.backends.begin(),
        serviceIt->second.backends.end(),
        [&](const BackendServer& backend) {
            return backendMatchesConnection(backend, connectionIt->second);
        });
    if (backendIt == serviceIt->second.backends.end() || !backendIt->healthy) {
        removeConnectionLocked(connectionIt);
        maintenanceCondition_.notify_one();
        return;
    }

    connectionIt->second.isActive = false;
    connectionIt->second.lastUsed = std::chrono::steady_clock::now();
    serviceIt->second.idleConnections.push_back(id);

    if (backendIt->activeConnections > 0) {
        --backendIt->activeConnections;
    }

    maintenanceCondition_.notify_one();
}

void ConnectionManager::closeConnection(ConnectionId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = connections_.find(id);
    if (it == connections_.end()) {
        return;
    }

    removeConnectionLocked(it);
    maintenanceCondition_.notify_one();
}

ConnectionPoolStats ConnectionManager::getPoolStats() const {
    std::lock_guard<std::mutex> lock(mutex_);

    ConnectionPoolStats stats;
    stats.totalConnections = connections_.size();
    stats.pendingRequests = pendingRequests_;

    for (const auto& entry : connections_) {
        if (entry.second.isActive) {
            ++stats.activeConnections;
        } else {
            ++stats.idleConnections;
        }
    }

    for (const auto& service : services_) {
        stats.maxConnections += service.second.maxPoolSize;
    }
    return stats;
}

ServicePoolStats ConnectionManager::getServiceStats(const std::string& service) const {
    std::lock_guard<std::mutex> lock(mutex_);
    ServicePoolStats stats;
    stats.service = service;

    const auto serviceIt = services_.find(service);
    if (serviceIt == services_.end()) {
        return stats;
    }

    stats.strategy = serviceIt->second.strategy;
    stats.circuitBreaker = serviceIt->second.circuitBreaker;
    stats.maxConnections = serviceIt->second.maxPoolSize;
    stats.pendingRequests = serviceIt->second.pendingRequests;
    stats.backendCount = serviceIt->second.backends.size();
    stats.healthyBackends = std::count_if(
        serviceIt->second.backends.begin(),
        serviceIt->second.backends.end(),
        [](const BackendServer& backend) { return backend.healthy; });

    for (const auto& entry : connections_) {
        if (entry.second.service != service) {
            continue;
        }

        ++stats.totalConnections;
        if (entry.second.isActive) {
            ++stats.activeConnections;
        } else {
            ++stats.idleConnections;
        }
    }

    return stats;
}

std::vector<std::string> ConnectionManager::getServices() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> names;
    names.reserve(services_.size());
    for (const auto& entry : services_) {
        names.push_back(entry.first);
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::vector<BackendServer> ConnectionManager::getBackendServers(const std::string& service) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = services_.find(service);
    return it == services_.end() ? std::vector<BackendServer>() : it->second.backends;
}

void ConnectionManager::addBackendServer(const std::string& service,
                                         const std::string& host,
                                         uint16_t port,
                                         int weight) {
    if (service.empty() || host.empty() || port == 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    ServiceState& serviceState = ensureServiceStateLocked(service);
    const auto it = std::find_if(serviceState.backends.begin(),
                                 serviceState.backends.end(),
                                 [&](const BackendServer& backend) {
                                     return backend.host == host && backend.port == port;
                                 });
    if (it != serviceState.backends.end()) {
        it->weight = std::max(1, weight);
        it->healthy = true;
        it->currentWeight = 0;
        return;
    }

    BackendServer backend;
    backend.host = host;
    backend.port = port;
    backend.weight = std::max(1, weight);
    backend.lastCheckTime = std::chrono::steady_clock::now();
    serviceState.backends.push_back(std::move(backend));
}

void ConnectionManager::removeBackendServer(const std::string& service,
                                            const std::string& host,
                                            uint16_t port) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto serviceIt = services_.find(service);
    if (serviceIt == services_.end()) {
        return;
    }

    auto& backends = serviceIt->second.backends;
    backends.erase(std::remove_if(backends.begin(),
                                  backends.end(),
                                  [&](const BackendServer& backend) {
                                      return backend.host == host && backend.port == port;
                                  }),
                   backends.end());
    if (serviceIt->second.roundRobinIndex >= backends.size() && !backends.empty()) {
        serviceIt->second.roundRobinIndex %= backends.size();
    }

    for (auto it = connections_.begin(); it != connections_.end();) {
        if (it->second.service == service &&
            it->second.remoteHost == host &&
            it->second.remotePort == port &&
            !it->second.isActive) {
            it = removeConnectionLocked(it);
            continue;
        }
        ++it;
    }
}

void ConnectionManager::updateBackendWeight(const std::string& service,
                                            const std::string& host,
                                            uint16_t port,
                                            int weight) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto serviceIt = services_.find(service);
    if (serviceIt == services_.end()) {
        return;
    }

    const auto it = std::find_if(serviceIt->second.backends.begin(),
                                 serviceIt->second.backends.end(),
                                 [&](const BackendServer& backend) {
                                     return backend.host == host && backend.port == port;
                                 });
    if (it == serviceIt->second.backends.end()) {
        return;
    }

    it->weight = std::max(1, weight);
    it->currentWeight = 0;
}

LoadBalancingStrategy ConnectionManager::getLoadBalancingStrategy(const std::string& service) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = services_.find(service);
    if (it == services_.end()) {
        return parseStrategy(config_.getString(Configuration::Option::LoadBalancingStrategy, "RoundRobin"));
    }
    return it->second.strategy;
}

void ConnectionManager::setLoadBalancingStrategy(const std::string& service,
                                                 LoadBalancingStrategy strategy) {
    std::lock_guard<std::mutex> lock(mutex_);
    ServiceState& serviceState = ensureServiceStateLocked(service);
    serviceState.strategy = strategy;
}

CircuitBreakerStats ConnectionManager::getCircuitBreakerStats(const std::string& service) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = services_.find(service);
    return it == services_.end() ? CircuitBreakerStats() : it->second.circuitBreaker;
}

void ConnectionManager::reportBackendStatus(const std::string& host, uint16_t port, bool success) {
    if (host.empty() || port == 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    const auto now = std::chrono::steady_clock::now();
    for (auto& service : services_) {
        for (auto& backend : service.second.backends) {
            if (backend.host == host && backend.port == port) {
                const bool wasHealthy = backend.healthy;
                applyBackendStatus(backend, success, failureThreshold_, now);
                if (wasHealthy && !backend.healthy) {
                    for (auto it = connections_.begin(); it != connections_.end();) {
                        if (it->second.service == service.first &&
                            it->second.remoteHost == host &&
                            it->second.remotePort == port &&
                            !it->second.isActive) {
                            it = removeConnectionLocked(it);
                            continue;
                        }
                        ++it;
                    }
                }
            }
        }
    }
}

void ConnectionManager::reportBackendStatus(const std::string& service,
                                            const std::string& host,
                                            uint16_t port,
                                            bool success) {
    if (service.empty() || host.empty() || port == 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    const auto serviceIt = services_.find(service);
    if (serviceIt == services_.end()) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    for (auto& backend : serviceIt->second.backends) {
        if (backend.host == host && backend.port == port) {
            const bool wasHealthy = backend.healthy;
            applyBackendStatus(backend, success, failureThreshold_, now);
            if (wasHealthy && !backend.healthy) {
                for (auto it = connections_.begin(); it != connections_.end();) {
                    if (it->second.service == service &&
                        it->second.remoteHost == host &&
                        it->second.remotePort == port &&
                        !it->second.isActive) {
                        it = removeConnectionLocked(it);
                        continue;
                    }
                    ++it;
                }
            }
            break;
        }
    }
}

void ConnectionManager::reportExecution(bool success) {
    if (!circuitBreakerEnabled_) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    const auto now = std::chrono::steady_clock::now();
    for (auto& service : services_) {
        applyExecutionResult(
            service.second.circuitBreaker, success, failureThreshold_, halfOpenAttempts_, now);
    }
}

void ConnectionManager::reportExecution(const std::string& service, bool success) {
    if (!circuitBreakerEnabled_) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = services_.find(service);
    if (it == services_.end()) {
        return;
    }

    applyExecutionResult(
        it->second.circuitBreaker, success, failureThreshold_, halfOpenAttempts_, std::chrono::steady_clock::now());
}

ConnectionManager::ServiceState& ConnectionManager::ensureServiceStateLocked(const std::string& service) {
    ServiceState& serviceState = services_[service];
    if (serviceState.maxPoolSize == 0) {
        serviceState.maxPoolSize =
            static_cast<size_t>(std::max(1, config_.getInt(Configuration::Option::MaxConnections, 1024)));
        serviceState.strategy =
            parseStrategy(config_.getString(Configuration::Option::LoadBalancingStrategy, "RoundRobin"));
    }
    return serviceState;
}

ConnectionId ConnectionManager::createConnectionLocked(const std::string& service,
                                                       BackendServer& backend,
                                                       std::chrono::steady_clock::time_point now) {
    ConnectionInfo info;
    info.id = nextConnectionId_.fetch_add(1, std::memory_order_relaxed);
    info.service = service;
    info.remoteHost = backend.host;
    info.remotePort = backend.port;
    info.createdAt = now;
    info.lastUsed = now;
    info.isActive = true;

    ++backend.activeConnections;
    connections_[info.id] = info;

    // Maintain O(1) per-service connection counter to avoid O(N) scans
    // in getServiceConnectionCountLocked on every acquireConnection().
    auto serviceIt = services_.find(service);
    if (serviceIt != services_.end()) {
        ++serviceIt->second.totalConnections;
    }
    return info.id;
}

std::unordered_map<ConnectionId, ConnectionInfo>::iterator ConnectionManager::removeConnectionLocked(
    std::unordered_map<ConnectionId, ConnectionInfo>::iterator it) {
    auto serviceIt = services_.find(it->second.service);
    if (serviceIt != services_.end()) {
        auto& serviceState = serviceIt->second;
        serviceState.idleConnections.erase(
            std::remove(serviceState.idleConnections.begin(), serviceState.idleConnections.end(), it->first),
            serviceState.idleConnections.end());

        auto backendIt = std::find_if(
            serviceState.backends.begin(),
            serviceState.backends.end(),
            [&](const BackendServer& backend) {
                return backend.host == it->second.remoteHost && backend.port == it->second.remotePort;
            });
        if (backendIt != serviceState.backends.end() && it->second.isActive &&
            backendIt->activeConnections > 0) {
            --backendIt->activeConnections;
        }
        // Keep the O(1) totalConnections counter in sync.
        if (serviceState.totalConnections > 0) {
            --serviceState.totalConnections;
        }
    }

    return connections_.erase(it);
}

void ConnectionManager::cleanupExpiredConnectionsLocked(std::chrono::steady_clock::time_point now) {
    for (auto it = connections_.begin(); it != connections_.end();) {
        if (it->second.isActive || idleTimeout_.count() == 0 || now - it->second.lastUsed <= idleTimeout_) {
            ++it;
            continue;
        }

        it = removeConnectionLocked(it);
    }

    for (auto& service : services_) {
        pruneIdleConnectionsLocked(service.second);
    }
}

void ConnectionManager::performHealthCheckLocked(std::chrono::steady_clock::time_point now) {
    for (auto& service : services_) {
        CircuitBreakerStats& breaker = service.second.circuitBreaker;
        if (breaker.state == CircuitBreakerState::Open &&
            (recoveryTimeout_.count() == 0 || now - breaker.lastFailureTime >= recoveryTimeout_)) {
            breaker.state = CircuitBreakerState::HalfOpen;
            breaker.successCount = 0;
            breaker.timeoutCount = 0;
        }

        for (auto& backend : service.second.backends) {
            if (!backend.healthy &&
                (recoveryTimeout_.count() == 0 || now - backend.lastFailureTime >= recoveryTimeout_)) {
                backend.healthy = true;
                backend.failureCount = 0;
                backend.currentWeight = 0;
            }
        }

        pruneIdleConnectionsLocked(service.second);
    }
}

void ConnectionManager::pruneIdleConnectionsLocked(ServiceState& serviceState) {
    serviceState.idleConnections.erase(
        std::remove_if(serviceState.idleConnections.begin(),
                       serviceState.idleConnections.end(),
                       [this](ConnectionId id) {
                           auto it = connections_.find(id);
                           return it == connections_.end() || it->second.isActive;
                       }),
        serviceState.idleConnections.end());
}

BackendServer* ConnectionManager::selectBackendLocked(ServiceState& serviceState,
                                                      std::string_view affinityKey) {
    std::vector<BackendServer*> healthyBackends;
    healthyBackends.reserve(serviceState.backends.size());
    for (auto& backend : serviceState.backends) {
        if (backend.healthy) {
            healthyBackends.push_back(&backend);
        }
    }

    if (healthyBackends.empty()) {
        return nullptr;
    }

    switch (serviceState.strategy) {
        case LoadBalancingStrategy::Random: {
            static thread_local std::mt19937 generator{std::random_device{}()};
            std::uniform_int_distribution<size_t> distribution(0, healthyBackends.size() - 1);
            return healthyBackends[distribution(generator)];
        }
        case LoadBalancingStrategy::LeastConnections: {
            return *std::min_element(healthyBackends.begin(),
                                     healthyBackends.end(),
                                     [](const BackendServer* lhs, const BackendServer* rhs) {
                                         if (lhs->activeConnections == rhs->activeConnections) {
                                             return lhs->weight > rhs->weight;
                                         }
                                         return lhs->activeConnections < rhs->activeConnections;
                                     });
        }
        case LoadBalancingStrategy::WeightedRoundRobin: {
            BackendServer* best = nullptr;
            int totalWeight = 0;
            for (BackendServer* backend : healthyBackends) {
                const int effectiveWeight = std::max(1, backend->weight);
                totalWeight += effectiveWeight;
                backend->currentWeight += effectiveWeight;
                if (best == nullptr || backend->currentWeight > best->currentWeight ||
                    (backend->currentWeight == best->currentWeight &&
                     backend->activeConnections < best->activeConnections)) {
                    best = backend;
                }
            }
            if (best != nullptr) {
                best->currentWeight -= totalWeight;
            }
            return best;
        }
        case LoadBalancingStrategy::IPHash: {
            if (affinityKey.empty()) {
                const size_t index = serviceState.roundRobinIndex++ % healthyBackends.size();
                return healthyBackends[index];
            }
            const size_t index = std::hash<std::string_view>{}(affinityKey) % healthyBackends.size();
            return healthyBackends[index];
        }
        case LoadBalancingStrategy::RoundRobin:
        default: {
            const size_t index = serviceState.roundRobinIndex++ % healthyBackends.size();
            return healthyBackends[index];
        }
    }
}

bool ConnectionManager::prepareCircuitBreakerLocked(ServiceState& serviceState,
                                                    std::chrono::steady_clock::time_point now) {
    if (!circuitBreakerEnabled_) {
        return true;
    }

    CircuitBreakerStats& breaker = serviceState.circuitBreaker;
    if (breaker.state == CircuitBreakerState::Open) {
        if (recoveryTimeout_.count() > 0 && now - breaker.lastFailureTime < recoveryTimeout_) {
            return false;
        }
        breaker.state = CircuitBreakerState::HalfOpen;
        breaker.successCount = 0;
        breaker.timeoutCount = 0;
    }

    if (breaker.state == CircuitBreakerState::HalfOpen &&
        getActiveConnectionCountLocked(serviceState) >= halfOpenAttempts_) {
        return false;
    }

    return true;
}

size_t ConnectionManager::getServiceConnectionCountLocked(std::string_view service) const {
    // O(1): read the per-service counter instead of scanning all connections.
    const auto it = services_.find(std::string(service));
    if (it == services_.end()) {
        return 0;
    }
    return it->second.totalConnections;
}

size_t ConnectionManager::getActiveConnectionCountLocked(const ServiceState& serviceState) {
    size_t activeConnections = 0;
    for (const auto& backend : serviceState.backends) {
        activeConnections += backend.activeConnections;
    }
    return activeConnections;
}

LoadBalancingStrategy ConnectionManager::parseStrategy(std::string_view strategyName) {
    const std::string normalized = normalizeStrategyName(strategyName);
    if (normalized == "random") {
        return LoadBalancingStrategy::Random;
    }
    if (normalized == "leastconnections") {
        return LoadBalancingStrategy::LeastConnections;
    }
    if (normalized == "weightedroundrobin") {
        return LoadBalancingStrategy::WeightedRoundRobin;
    }
    if (normalized == "iphash") {
        return LoadBalancingStrategy::IPHash;
    }
    return LoadBalancingStrategy::RoundRobin;
}

void ConnectionManager::maintenanceLoop() {
    const auto interval =
        healthCheckInterval_.count() > 0 ? healthCheckInterval_ : std::chrono::milliseconds(1000);
    while (running_.load(std::memory_order_acquire)) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (maintenanceCondition_.wait_for(
                lock,
                interval,
                [this]() { return !running_.load(std::memory_order_acquire); })) {
            break;
        }

        const auto now = std::chrono::steady_clock::now();
        cleanupExpiredConnectionsLocked(now);
        performHealthCheckLocked(now);
    }
}

} // namespace FastNet
