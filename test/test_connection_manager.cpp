#include "FastNet/FastNet.h"
#include "TestSupport.h"

#include <iostream>

namespace {

void testUnhealthyBackendDoesNotReuseIdleConnection() {
    FastNet::Configuration config;
    config.set(FastNet::Configuration::Option::MaxConnections, 4);
    config.set(FastNet::Configuration::Option::FailureThreshold, 1);
    config.set(FastNet::Configuration::Option::CircuitBreakerEnabled, true);

    FastNet::ConnectionManager manager(config);
    manager.addBackendServer("svc-unhealthy", "127.0.0.1", 8080);

    const FastNet::ConnectionId connectionId = manager.acquireConnection("svc-unhealthy");
    FASTNET_TEST_ASSERT(connectionId != 0);
    manager.releaseConnection(connectionId);

    manager.reportBackendStatus("svc-unhealthy", "127.0.0.1", 8080, false);

    FASTNET_TEST_ASSERT_EQ(manager.acquireConnection("svc-unhealthy"), static_cast<FastNet::ConnectionId>(0));
}

void testRemovedBackendDoesNotReturnReleasedConnectionToIdlePool() {
    FastNet::Configuration config;
    config.set(FastNet::Configuration::Option::MaxConnections, 4);

    FastNet::ConnectionManager manager(config);
    manager.addBackendServer("svc-removed", "127.0.0.1", 9090);

    const FastNet::ConnectionId connectionId = manager.acquireConnection("svc-removed");
    FASTNET_TEST_ASSERT(connectionId != 0);

    manager.removeBackendServer("svc-removed", "127.0.0.1", 9090);
    manager.releaseConnection(connectionId);

    const FastNet::ServicePoolStats stats = manager.getServiceStats("svc-removed");
    FASTNET_TEST_ASSERT_EQ(stats.totalConnections, static_cast<size_t>(0));
    FASTNET_TEST_ASSERT_EQ(stats.idleConnections, static_cast<size_t>(0));
    FASTNET_TEST_ASSERT_EQ(manager.acquireConnection("svc-removed"), static_cast<FastNet::ConnectionId>(0));
}

} // namespace

int main() {
    testUnhealthyBackendDoesNotReuseIdleConnection();
    testRemovedBackendDoesNotReturnReleasedConnectionToIdlePool();

    std::cout << "connection manager tests passed" << '\n';
    return 0;
}
