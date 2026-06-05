# FastNet Cookbook

This Cookbook is organized by "What task do I need to accomplish". To understand the overall design, read [USER_GUIDE.md](USER_GUIDE.md) first; to look up method names, check [API_REFERENCE.md](API_REFERENCE.md) directly.

## 1. Quick Selection

| Task | Recommended Combination | Description |
| --- | --- | --- |
| High-performance binary service | `TcpServer` + owned receive callback | Private protocols, RPC, proxy forwarding |
| Upstream TCP client | `TcpClient` / `TcpConnectionPool` | Persistent connections or fixed backend reuse |
| HTTP API + static files | `HttpServer` | REST APIs, health checks, static resources |
| Call HTTP/HTTPS APIs | `HttpClient` | Standard request-response client |
| Real-time push | `WebSocketServer` / `WebSocketClient` | Subscriptions, chats, two-way messaging |
| Broadcast & keep-alive probes | `UdpSocket` | Lightweight messaging with tolerance for packet loss |
| Multi-backend scheduling | `ConnectionManager` + pool/client | Load balancing, circuit breakers, health status |

## 2. High-Performance TCP Echo / Private Protocol Service

**Applicable to**:
- Internal binary RPC.
- Private protocols between a gateway and backend.
- Single-machine high-throughput forwarding.

**Best Practices**:
1. Share a single `IoService` within a process where possible.
2. Use `setOwnedDataReceivedCallback()` for receiving packets.
3. Use `std::move(data)` directly for forwarding or echo.
4. Do not set connection read timeouts too short for persistent connections.

```cpp
FastNet::TcpServer server(FastNet::getGlobalIoService());

server.setOwnedDataReceivedCallback([&server](FastNet::ConnectionId id, FastNet::Buffer&& data) {
    server.sendToClient(id, std::move(data));
});

server.setServerErrorCallback([](const FastNet::Error& error) {
    // Log error.toString()
});

server.start(9000);
```

You can also configure:
- `setMaxConnections()`
- `setConnectionTimeout()`
- `setWriteTimeout()`
- `closeClientAfterPendingWrites()`

Reference: [../../examples/tcp_echo_server.cpp](../../examples/tcp_echo_server.cpp)

## 3. Upstream TCP Client or Proxy Connector

**Applicable to**:
- RPC client initiating connections to a backend.
- TCP proxy.
- Persistent upstream connections.

```cpp
FastNet::TcpClient client(FastNet::getGlobalIoService());
client.setConnectTimeout(5000);
client.setSharedDataReceivedCallback([](const std::shared_ptr<const FastNet::Buffer>& data) {
    // Parse or forward data
});

client.connect("127.0.0.1", 9000, [&](bool success, const std::string&) {
    if (success) {
        client.send(std::string("ping"));
    }
});
```

If the upstream is fixed and requests are frequent, do not recreate `TcpClient` every time; use `TcpConnectionPool` instead.

## 4. HTTP API + Static Files

**Applicable to**:
- Admin panels.
- Health checks.
- Simple REST APIs.
- Embedded static pages.

```cpp
FastNet::HttpServer server(FastNet::getGlobalIoService());

server.registerGet("/healthz", [](const FastNet::HttpRequest&, FastNet::HttpResponse& response) {
    response.statusCode = 200;
    response.statusMessage = "OK";
    response.headers["Content-Type"] = "text/plain; charset=utf-8";
    response.body = "ok";
});

server.registerPost("/api/echo", [](const FastNet::HttpRequest& request, FastNet::HttpResponse& response) {
    response.statusCode = 200;
    response.statusMessage = "OK";
    response.headers["Content-Type"] = "application/json; charset=utf-8";
    response.body = request.body;
});

server.registerStaticFileHandler("/static", "./public");
server.setMaxRequestSize(2 * 1024 * 1024);
server.setStaticFileCacheLimit(8 * 1024 * 1024);
server.start(8080);
```

**Recommendations**:
- Explicitly set `Content-Type`.
- Set an upper limit for the request body size.
- Serve static resources via `registerStaticFileHandler()`.
- Use file-backed responses for large file deliveries.

Reference: [../../examples/http_static_server.cpp](../../examples/http_static_server.cpp)

## 5. Calling HTTP/HTTPS APIs

**Applicable to**:
- Invoking third-party REST APIs.
- Inter-service HTTP requests.
- Health check probes.

```cpp
FastNet::HttpClient client(FastNet::getGlobalIoService());
client.setConnectTimeout(5000);
client.setRequestTimeout(5000);
client.setReadTimeout(5000);
client.setFollowRedirects(true);
client.setMaxRedirects(5);

FastNet::SSLConfig ssl;
ssl.enableSSL = true;
ssl.verifyPeer = true;
ssl.caFile = "ca.crt";
ssl.hostnameVerification = "api.example.com";
client.setSSLConfig(ssl);

client.connect("https://api.example.com", [&](bool success, const std::string&) {
    if (!success) {
        return;
    }

    client.get("/v1/status", {}, [](const FastNet::HttpResponse& response) {
        // Handle response
    });
});
```

**Notes**:
- `HttpClient` currently uses HTTP/1.1 keep-alive.
- Effective in-flight per connection is `1`.
- To increase overall throughput, scale the connection count or build a connection pool at the application layer.

Reference: [../../examples/http_get_client.cpp](../../examples/http_get_client.cpp)

## 6. WebSocket Push Service

**Applicable to**:
- Chat rooms.
- Subscription pushing.
- Real-time status synchronization.

```cpp
FastNet::WebSocketServer server(FastNet::getGlobalIoService());

server.setMessageCallback([&server](FastNet::ConnectionId id, const std::string& message) {
    server.sendTextToClient(id, message);
});

server.setOwnedBinaryCallback([&server](FastNet::ConnectionId id, FastNet::Buffer&& data) {
    server.sendBinaryToClient(id, data);
});

server.setPingInterval(30000);
server.start(8081);
```

**Recommendations**:
- Route text messages via `setMessageCallback()`.
- Route binary forwarding via owned callbacks.
- Attempt close handshakes on connection shutdowns.

Reference: [../../examples/websocket_echo_server.cpp](../../examples/websocket_echo_server.cpp)

## 7. Running HTTP and WebSocket in the Same Process

The recommended robust way is running them in the same process and same `IoService` on different ports:

```cpp
auto& io = FastNet::getGlobalIoService();

FastNet::HttpServer http(io);
FastNet::WebSocketServer ws(io);

http.registerGet("/healthz", [](const FastNet::HttpRequest&, FastNet::HttpResponse& response) {
    response.statusCode = 200;
    response.body = "ok";
});
http.start(8080);

ws.setMessageCallback([](FastNet::ConnectionId, const std::string&) {
    // Push logic
});
ws.start(8081);
```

To expose them under a unified entrypoint, place Nginx or an API gateway in front as a reverse proxy. Do not assume same-port upgrade routing is built into the library.

## 8. Enabling TLS

**Server**:

```cpp
FastNet::SSLConfig ssl;
ssl.enableSSL = true;
ssl.certificateFile = "server.crt";
ssl.privateKeyFile = "server.key";
ssl.verifyPeer = false;

server.start(8443, "0.0.0.0", ssl);
```

**Client**:

```cpp
FastNet::SSLConfig ssl;
ssl.enableSSL = true;
ssl.caFile = "ca.crt";
ssl.verifyPeer = true;
ssl.hostnameVerification = "api.example.com";
```

**Applicable APIs**:
- `TcpServer::start(..., ssl)`
- `TcpClient::connect(..., ssl)`
- `HttpServer::start(..., ssl)`
- `HttpClient::setSSLConfig(ssl)`
- `WebSocketServer::start(..., ssl)`
- `WebSocketClient::setSSLConfig(ssl)`

**Compilation requires flags**:

```powershell
build.bat --ssl --release-only
```

```bash
./build.sh --ssl --release-only
```

## 9. Connection Pooling

**Applicable to**:
- Fixed upstream backends.
- High frequency requests.
- Significant connection establishment overhead.

```cpp
FastNet::TcpConnectionPoolOptions options;
options.minConnections = 2;
options.maxConnections = 16;
options.connectionTimeout = 5000;
options.acquireTimeout = 5000;

FastNet::TcpConnectionPool pool(FastNet::getGlobalIoService(), "127.0.0.1", 9000, options);
pool.initialize();

pool.acquire([](const FastNet::Error& error, std::shared_ptr<FastNet::PooledConnection> conn) {
    if (error.isFailure() || !conn) {
        return;
    }
    conn->getClient()->send(std::string("ping"));
});
```

Call `release()` to return connections to the pool, and do not hold a `PooledConnection` exclusively for long periods outside the pool.

## 10. Load Balancing and Circuit Breakers

**Applicable to**:
- Routing requests across multiple backend instances discovered via service discovery.
- Requirements for RoundRobin, LeastConnections, WeightedRoundRobin, or IPHash algorithms.
- Basic circuit breakers and health monitoring.

```cpp
FastNet::Configuration config;
FastNet::ConnectionManager manager(config);
manager.initialize();

manager.addBackendServer("order", "10.0.0.11", 9000, 3);
manager.addBackendServer("order", "10.0.0.12", 9000, 1);
manager.setLoadBalancingStrategy("order", FastNet::LoadBalancingStrategy::WeightedRoundRobin);

const FastNet::ConnectionId id = manager.acquireConnection("order");
manager.releaseConnection(id);
```

**Division of labor**:
- `ConnectionManager` chooses backends and tracks their health.
- `TcpConnectionPool` handles connection reuse within each selected backend.

## 11. UDP Broadcasting, Heartbeats, and Light Messages

```cpp
FastNet::UdpSocket socket(FastNet::getGlobalIoService());
socket.setBroadcast(true);
socket.setDataReceivedCallback([](const FastNet::Address& sender, const FastNet::Buffer& data) {
    // Process message
});

socket.bind(9900);
socket.startReceive();
socket.sendTo(FastNet::Address("255.255.255.255", 9900), "ping");
```

UDP does not guarantee delivery, order, or connection state. If reliability is a requirement, fall back to TCP or WebSocket.

## 12. FAQ

### Why is HTTP benchmark performance lower than TCP?
HTTP involves parsing headers, routing, request boundaries, response construction, and keep-alive management. It is natural that it is slower than raw TCP.

### Why does the `soak` profile run for a long time?
The `soak` profile is for long-term stability validation. A standard soak test runs for ~15 minutes and may take longer depending on target machine resources.

### Can HTTP and WebSocket share the same port?
Not recommended by default. Use different ports under the same process/`IoService`, and unify them via a reverse proxy.

### When should I enable TLS?
By default for external services. Decide for internal services based on your trust boundaries and compliance rules. It is recommended to benchmark both plain and TLS modes as the overhead difference is noticeable.

## 13. Next Steps

- [USER_GUIDE.md](USER_GUIDE.md)
- [API_REFERENCE.md](API_REFERENCE.md)
- [VALIDATION_CHECKLIST.md](VALIDATION_CHECKLIST.md)
- [RELEASE_STATUS.md](RELEASE_STATUS.md)
