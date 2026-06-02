/**
 * @file ConnectionManager.cpp
 * @brief FastNet service-level connection manager
 */
#include "ConnectionManager.h"
#include "Configuration.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <random>
#include <thread>
#include <unordered_map>
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

struct ConnectionManager::Impl {
    struct ServiceState {
        std::vector<BackendServer> backends;
        std::deque<ConnectionId> idleConnections;
        LoadBalancingStrategy strategy = LoadBalancingStrategy::RoundRobin;
        CircuitBreakerStats circuitBreaker;
        size_t roundRobinIndex   = 0;
        size_t maxPoolSize       = 0;
        size_t pendingRequests   = 0;
        size_t totalConnections  = 0;
    };

    Configuration config;
    mutable std::mutex mutex;
    std::unordered_map<ConnectionId, ConnectionInfo> connections;
    std::unordered_map<std::string, ServiceState> services;
    std::atomic<ConnectionId> nextConnectionId{1};
    std::atomic<bool> running{false};
    std::thread maintenanceThread;
    std::condition_variable maintenanceCondition;
    size_t pendingRequests = 0;
    std::chrono::milliseconds idleTimeout;
    std::chrono::milliseconds healthCheckInterval;
    std::chrono::milliseconds recoveryTimeout;
    bool circuitBreakerEnabled = true;
    size_t failureThreshold = 5;
    size_t halfOpenAttempts = 3;

    explicit Impl(const Configuration& cfg)
        : config(cfg),
          idleTimeout(config.getInt(Configuration::Option::ConnectionTimeout, 30000)),
          healthCheckInterval(config.getInt(Configuration::Option::HealthCheckInterval, 30000)),
          recoveryTimeout(config.getInt(Configuration::Option::RecoveryTimeout, 60000)),
          circuitBreakerEnabled(config.getBool(Configuration::Option::CircuitBreakerEnabled, true)),
          failureThreshold(static_cast<size_t>(std::max(1, config.getInt(Configuration::Option::FailureThreshold, 5)))),
          halfOpenAttempts(static_cast<size_t>(std::max(1, config.getInt(Configuration::Option::HalfOpenAttempts, 3)))) {}

    ServiceState& ensureServiceStateLocked(const std::string& service) {
        ServiceState& serviceState = services[service];
        if (serviceState.maxPoolSize == 0) {
            serviceState.maxPoolSize =
                static_cast<size_t>(std::max(1, config.getInt(Configuration::Option::MaxConnections, 1024)));
            serviceState.strategy =
                parseStrategy(config.getString(Configuration::Option::LoadBalancingStrategy, "RoundRobin"));
        }
        return serviceState;
    }

    ConnectionId createConnectionLocked(const std::string& service,
                                        BackendServer& backend,
                                        std::chrono::steady_clock::time_point now) {
        ConnectionInfo info;
        info.id = nextConnectionId.fetch_add(1, std::memory_order_relaxed);
        info.service = service;
        info.remoteHost = backend.host;
        info.remotePort = backend.port;
        info.createdAt = now;
        info.lastUsed = now;
        info.isActive = true;

        ++backend.activeConnections;
        connections[info.id] = info;

        auto serviceIt = services.find(service);
        if (serviceIt != services.end()) {
            ++serviceIt->second.totalConnections;
        }
        return info.id;
    }

    std::unordered_map<ConnectionId, ConnectionInfo>::iterator removeConnectionLocked(
        std::unordered_map<ConnectionId, ConnectionInfo>::iterator it) {
        auto serviceIt = services.find(it->second.service);
        if (serviceIt != services.end()) {
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
            if (serviceState.totalConnections > 0) {
                --serviceState.totalConnections;
            }
        }

        return connections.erase(it);
    }

    void cleanupExpiredConnectionsLocked(std::chrono::steady_clock::time_point now) {
        for (auto it = connections.begin(); it != connections.end();) {
            if (it->second.isActive || idleTimeout.count() == 0 || now - it->second.lastUsed <= idleTimeout) {
                ++it;
                continue;
            }

            it = removeConnectionLocked(it);
        }

        for (auto& service : services) {
            pruneIdleConnectionsLocked(service.second);
        }
    }

    void performHealthCheckLocked(std::chrono::steady_clock::time_point now) {
        for (auto& service : services) {
            CircuitBreakerStats& breaker = service.second.circuitBreaker;
            if (breaker.state == CircuitBreakerState::Open &&
                (recoveryTimeout.count() == 0 || now - breaker.lastFailureTime >= recoveryTimeout)) {
                breaker.state = CircuitBreakerState::HalfOpen;
                breaker.successCount = 0;
                breaker.timeoutCount = 0;
            }

            for (auto& backend : service.second.backends) {
                if (!backend.healthy &&
                    (recoveryTimeout.count() == 0 || now - backend.lastFailureTime >= recoveryTimeout)) {
                    backend.healthy = true;
                    backend.failureCount = 0;
                    backend.currentWeight = 0;
                }
            }

            pruneIdleConnectionsLocked(service.second);
        }
    }

    void pruneIdleConnectionsLocked(ServiceState& serviceState) {
        serviceState.idleConnections.erase(
            std::remove_if(serviceState.idleConnections.begin(),
                           serviceState.idleConnections.end(),
                           [this](ConnectionId id) {
                               auto it = connections.find(id);
                               return it == connections.end() || it->second.isActive;
                           }),
            serviceState.idleConnections.end());
    }

    BackendServer* selectBackendLocked(ServiceState& serviceState, std::string_view affinityKey) {
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

    bool prepareCircuitBreakerLocked(ServiceState& serviceState, std::chrono::steady_clock::time_point now) {
        if (!circuitBreakerEnabled) {
            return true;
        }

        CircuitBreakerStats& breaker = serviceState.circuitBreaker;
        if (breaker.state == CircuitBreakerState::Open) {
            if (recoveryTimeout.count() > 0 && now - breaker.lastFailureTime < recoveryTimeout) {
                return false;
            }
            breaker.state = CircuitBreakerState::HalfOpen;
            breaker.successCount = 0;
            breaker.timeoutCount = 0;
        }

        if (breaker.state == CircuitBreakerState::HalfOpen &&
            getActiveConnectionCountLocked(serviceState) >= halfOpenAttempts) {
            return false;
        }

        return true;
    }

    size_t getServiceConnectionCountLocked(std::string_view service) const {
        const auto it = services.find(std::string(service));
        if (it == services.end()) {
            return 0;
        }
        return it->second.totalConnections;
    }

    static size_t getActiveConnectionCountLocked(const ServiceState& serviceState) {
        size_t activeConnections = 0;
        for (const auto& backend : serviceState.backends) {
            activeConnections += backend.activeConnections;
        }
        return activeConnections;
    }

    static LoadBalancingStrategy parseStrategy(std::string_view strategyName) {
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

    void maintenanceLoop() {
        const auto interval =
            healthCheckInterval.count() > 0 ? healthCheckInterval : std::chrono::milliseconds(1000);
        while (running.load(std::memory_order_acquire)) {
            std::unique_lock<std::mutex> lock(mutex);
            if (maintenanceCondition.wait_for(
                    lock,
                    interval,
                    [this]() { return !running.load(std::memory_order_acquire); })) {
                break;
            }

            const auto now = std::chrono::steady_clock::now();
            cleanupExpiredConnectionsLocked(now);
            performHealthCheckLocked(now);
        }
    }
};

ConnectionManager::ConnectionManager(const Configuration& config)
    : impl_(std::make_unique<Impl>(config)) {}

ConnectionManager::~ConnectionManager() {
    cleanup();
}

bool ConnectionManager::initialize() {
    bool expected = false;
    if (!impl_->running.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return true;
    }

    impl_->maintenanceThread = std::thread(&Impl::maintenanceLoop, impl_.get());
    return true;
}

void ConnectionManager::cleanup() {
    const bool wasRunning = impl_->running.exchange(false, std::memory_order_acq_rel);
    if (wasRunning) {
        impl_->maintenanceCondition.notify_all();
        if (impl_->maintenanceThread.joinable()) {
            impl_->maintenanceThread.join();
        }
    }

    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->connections.clear();
    impl_->services.clear();
    impl_->pendingRequests = 0;
}

ConnectionId ConnectionManager::acquireConnection(const std::string& service) {
    return acquireConnection(service, "");
}

ConnectionId ConnectionManager::acquireConnection(const std::string& service,
                                                  const std::string& affinityKey) {
    if (service.empty()) {
        return 0;
    }

    std::lock_guard<std::mutex> lock(impl_->mutex);
    Impl::ServiceState& serviceState = impl_->ensureServiceStateLocked(service);
    ++impl_->pendingRequests;
    ++serviceState.pendingRequests;

    const auto finish = [&](ConnectionId id) {
        if (impl_->pendingRequests > 0) {
            --impl_->pendingRequests;
        }
        if (serviceState.pendingRequests > 0) {
            --serviceState.pendingRequests;
        }
        return id;
    };

    const auto now = std::chrono::steady_clock::now();
    if (!impl_->prepareCircuitBreakerLocked(serviceState, now)) {
        return finish(0);
    }

    while (!serviceState.idleConnections.empty()) {
        const ConnectionId candidateId = serviceState.idleConnections.front();
        serviceState.idleConnections.pop_front();

        const auto connectionIt = impl_->connections.find(candidateId);
        if (connectionIt == impl_->connections.end()) {
            continue;
        }

        if (connectionIt->second.isActive) {
            continue;
        }

        if (impl_->idleTimeout.count() > 0 && now - connectionIt->second.lastUsed > impl_->idleTimeout) {
            impl_->removeConnectionLocked(connectionIt);
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
            impl_->removeConnectionLocked(connectionIt);
            continue;
        }

        ++backendIt->activeConnections;

        return finish(candidateId);
    }

    if (impl_->getServiceConnectionCountLocked(service) >= serviceState.maxPoolSize) {
        return finish(0);
    }

    BackendServer* backend = impl_->selectBackendLocked(serviceState, affinityKey);
    if (backend == nullptr) {
        return finish(0);
    }

    return finish(impl_->createConnectionLocked(service, *backend, now));
}

void ConnectionManager::releaseConnection(ConnectionId id) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto connectionIt = impl_->connections.find(id);
    if (connectionIt == impl_->connections.end() || !connectionIt->second.isActive) {
        return;
    }

    auto serviceIt = impl_->services.find(connectionIt->second.service);
    if (serviceIt == impl_->services.end()) {
        impl_->removeConnectionLocked(connectionIt);
        return;
    }

    auto backendIt = std::find_if(
        serviceIt->second.backends.begin(),
        serviceIt->second.backends.end(),
        [&](const BackendServer& backend) {
            return backendMatchesConnection(backend, connectionIt->second);
        });
    if (backendIt == serviceIt->second.backends.end() || !backendIt->healthy) {
        impl_->removeConnectionLocked(connectionIt);
        impl_->maintenanceCondition.notify_one();
        return;
    }

    connectionIt->second.isActive = false;
    connectionIt->second.lastUsed = std::chrono::steady_clock::now();
    serviceIt->second.idleConnections.push_back(id);

    if (backendIt->activeConnections > 0) {
        --backendIt->activeConnections;
    }

    impl_->maintenanceCondition.notify_one();
}

void ConnectionManager::closeConnection(ConnectionId id) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto it = impl_->connections.find(id);
    if (it == impl_->connections.end()) {
        return;
    }

    impl_->removeConnectionLocked(it);
    impl_->maintenanceCondition.notify_one();
}

ConnectionPoolStats ConnectionManager::getPoolStats() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);

    ConnectionPoolStats stats;
    stats.totalConnections = impl_->connections.size();
    stats.pendingRequests = impl_->pendingRequests;

    for (const auto& entry : impl_->connections) {
        if (entry.second.isActive) {
            ++stats.activeConnections;
        } else {
            ++stats.idleConnections;
        }
    }

    for (const auto& service : impl_->services) {
        stats.maxConnections += service.second.maxPoolSize;
    }
    return stats;
}

ServicePoolStats ConnectionManager::getServiceStats(const std::string& service) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    ServicePoolStats stats;
    stats.service = service;

    const auto serviceIt = impl_->services.find(service);
    if (serviceIt == impl_->services.end()) {
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

    for (const auto& entry : impl_->connections) {
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
    std::lock_guard<std::mutex> lock(impl_->mutex);
    std::vector<std::string> names;
    names.reserve(impl_->services.size());
    for (const auto& entry : impl_->services) {
        names.push_back(entry.first);
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::vector<BackendServer> ConnectionManager::getBackendServers(const std::string& service) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto it = impl_->services.find(service);
    return it == impl_->services.end() ? std::vector<BackendServer>() : it->second.backends;
}

void ConnectionManager::addBackendServer(const std::string& service,
                                         const std::string& host,
                                         uint16_t port,
                                         int weight) {
    if (service.empty() || host.empty() || port == 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(impl_->mutex);
    Impl::ServiceState& serviceState = impl_->ensureServiceStateLocked(service);
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
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto serviceIt = impl_->services.find(service);
    if (serviceIt == impl_->services.end()) {
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

    for (auto it = impl_->connections.begin(); it != impl_->connections.end();) {
        if (it->second.service == service &&
            it->second.remoteHost == host &&
            it->second.remotePort == port &&
            !it->second.isActive) {
            it = impl_->removeConnectionLocked(it);
            continue;
        }
        ++it;
    }
}

void ConnectionManager::updateBackendWeight(const std::string& service,
                                            const std::string& host,
                                            uint16_t port,
                                            int weight) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto serviceIt = impl_->services.find(service);
    if (serviceIt == impl_->services.end()) {
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
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto it = impl_->services.find(service);
    if (it == impl_->services.end()) {
        return Impl::parseStrategy(impl_->config.getString(Configuration::Option::LoadBalancingStrategy, "RoundRobin"));
    }
    return it->second.strategy;
}

void ConnectionManager::setLoadBalancingStrategy(const std::string& service,
                                                 LoadBalancingStrategy strategy) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    Impl::ServiceState& serviceState = impl_->ensureServiceStateLocked(service);
    serviceState.strategy = strategy;
}

CircuitBreakerStats ConnectionManager::getCircuitBreakerStats(const std::string& service) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto it = impl_->services.find(service);
    return it == impl_->services.end() ? CircuitBreakerStats() : it->second.circuitBreaker;
}

void ConnectionManager::reportBackendStatus(const std::string& host, uint16_t port, bool success) {
    if (host.empty() || port == 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto now = std::chrono::steady_clock::now();
    for (auto& service : impl_->services) {
        for (auto& backend : service.second.backends) {
            if (backend.host == host && backend.port == port) {
                const bool wasHealthy = backend.healthy;
                applyBackendStatus(backend, success, impl_->failureThreshold, now);
                if (wasHealthy && !backend.healthy) {
                    for (auto it = impl_->connections.begin(); it != impl_->connections.end();) {
                        if (it->second.service == service.first &&
                            it->second.remoteHost == host &&
                            it->second.remotePort == port &&
                            !it->second.isActive) {
                            it = impl_->removeConnectionLocked(it);
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

    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto serviceIt = impl_->services.find(service);
    if (serviceIt == impl_->services.end()) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    for (auto& backend : serviceIt->second.backends) {
        if (backend.host == host && backend.port == port) {
            const bool wasHealthy = backend.healthy;
            applyBackendStatus(backend, success, impl_->failureThreshold, now);
            if (wasHealthy && !backend.healthy) {
                for (auto it = impl_->connections.begin(); it != impl_->connections.end();) {
                    if (it->second.service == service &&
                        it->second.remoteHost == host &&
                        it->second.remotePort == port &&
                        !it->second.isActive) {
                        it = impl_->removeConnectionLocked(it);
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
    if (!impl_->circuitBreakerEnabled) {
        return;
    }

    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto now = std::chrono::steady_clock::now();
    for (auto& service : impl_->services) {
        applyExecutionResult(
            service.second.circuitBreaker, success, impl_->failureThreshold, impl_->halfOpenAttempts, now);
    }
}

void ConnectionManager::reportExecution(const std::string& service, bool success) {
    if (!impl_->circuitBreakerEnabled) {
        return;
    }

    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto it = impl_->services.find(service);
    if (it == impl_->services.end()) {
        return;
    }

    applyExecutionResult(
        it->second.circuitBreaker, success, impl_->failureThreshold, impl_->halfOpenAttempts, std::chrono::steady_clock::now());
}

} // namespace FastNet
