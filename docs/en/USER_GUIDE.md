# FastNet User Guide

This guide is for developers integrating FastNet into their applications. It focuses on module selection, minimal usage patterns, and production configurations.

If you already know what you want to achieve, go directly to [COOKBOOK.md](COOKBOOK.md). To look up public methods, refer to [API_REFERENCE.md](API_REFERENCE.md).

## 1. Module Selection

| Requirement | Recommended Module | Typical Use Case |
| --- | --- | --- |
| Raw binary persistent connection | `TcpServer` / `TcpClient` | Private protocols, internal RPC, proxy forwarding |
| Connectionless small messages | `UdpSocket` | Broadcasting, keep-alive heartbeats, status reporting |
| HTTP service | `HttpServer` | REST APIs, static files, health checks |
| HTTP client | `HttpClient` | Calling third-party APIs, internal HTTP services |
| Long-connection push | `WebSocketServer` / `WebSocketClient` | Chat, pub/sub, real-time messaging |
| Reusable upstream connection | `TcpConnectionPool` | Reduces repeat connection establishment overhead |
| Multi-backend scheduling | `ConnectionManager` | Client-side load balancing, circuit breakers, health checks |

Quick guide:
- Low overhead with custom protocols: **TCP**.
- Interfaces with browsers, curl, Nginx, or API Gateways: **HTTP / WebSocket**.
- Send-and-forget packets (loss-tolerant): **UDP**.
- Frequent connections to the same upstream: **TcpConnectionPool**.
- Routing across multiple backends: **ConnectionManager**.

## 2. Lifecycle Management

Most objects work around the same `IoService`. Recommended pattern:

1. Call `FastNet::initialize()` at process startup.
2. Get the global `IoService` via `FastNet::getGlobalIoService()`.
3. Create server, client, pool, or manager on this `IoService`.
4. Call `FastNet::cleanup()` after stopping all objects.

Minimal skeleton:

```cpp
#include "FastNet/FastNet.h"

int main() {
    if (FastNet::initialize() != FastNet::ErrorCode::Success) {
        return 1;
    }

    auto& ioService = FastNet::getGlobalIoService();

    // Create TcpServer / HttpServer / WebSocketServer / TcpClient etc.

    FastNet::cleanup();
    return 0;
}
```

## 3. Basic Types

### `Address`

```cpp
FastNet::Address a1("127.0.0.1", 9000);
FastNet::Address a2("localhost", 8080);
FastNet::Address a3("::1", 8081);
```

Common methods:
- `toString()`
- `isValid()`
- `isIPv6()`
- `isLoopback()`
- `isAnyAddress()`
- `Address::parse(endpoint, defaultPort)`

### `Buffer`

`Buffer` is an alias for `std::vector<uint8_t>`, suitable for TCP, UDP, and WebSocket binary frames.

Recommendations:
- Text-based protocols: `std::string` / `std::string_view`
- Binary-based protocols: `Buffer`
- Forwarding or broadcasting: Prefer move/shared interfaces to reduce copying.

### `SSLConfig`

Minimal server configuration:

```cpp
FastNet::SSLConfig ssl;
ssl.enableSSL = true;
ssl.certificateFile = "server.crt";
ssl.privateKeyFile = "server.key";
ssl.verifyPeer = false;
```

Production client configuration:

```cpp
FastNet::SSLConfig ssl;
ssl.enableSSL = true;
ssl.caFile = "ca.crt";
ssl.verifyPeer = true;
ssl.hostnameVerification = "api.example.com";
```

Note: TLS features require compiling with `FASTNET_ENABLE_SSL=ON`.

## 4. Callbacks and Data Ownership

FastNet offers three categories of receive callbacks:

- `const Buffer&`: Parse and consume immediately.
- `Buffer&&`: Forward directly or move into a business queue.
- `shared_ptr<const Buffer>`: Distribute the same data to multiple downstream connections.

Recommendations:
- Echo / proxy: Prefer `setOwnedDataReceivedCallback()`.
- Broadcasting: Prefer shared paths.
- HTTP/WebSocket text workloads: Handle strings or structs at the application layer without copying the underlying buffer.

## 5. Minimal Usage

### TCP Server

```cpp
#include "FastNet/FastNet.h"
#include <iostream>

int main() {
    if (FastNet::initialize() != FastNet::ErrorCode::Success) {
        return 1;
    }

    auto& ioService = FastNet::getGlobalIoService();
    FastNet::TcpServer server(ioService);

    server.setClientConnectedCallback([](FastNet::ConnectionId id, const FastNet::Address& addr) {
        std::cout << "client " << id << " connected from " << addr.toString() << '\n';
    });

    server.setOwnedDataReceivedCallback([&server](FastNet::ConnectionId id, FastNet::Buffer&& data) {
        server.sendToClient(id, std::move(data));
    });

    const FastNet::Error result = server.start(9000);
    if (result.isFailure()) {
        std::cerr << result.toString() << '\n';
    }

    std::string line;
    std::getline(std::cin, line);
    server.stop();
    FastNet::cleanup();
}
```

Reference: [../../examples/tcp_echo_server.cpp](../../examples/tcp_echo_server.cpp)

### TCP Client

```cpp
FastNet::TcpClient client(FastNet::getGlobalIoService());
client.setConnectTimeout(5000);
client.setReadTimeout(0);
client.setWriteTimeout(0);

client.setSharedDataReceivedCallback([](const std::shared_ptr<const FastNet::Buffer>& data) {
    // Parse or forward data
});

client.setErrorCallback([](FastNet::ErrorCode, const std::string& message) {
    // Log error
});

client.connect("127.0.0.1", 9000, [&](bool success, const std::string&) {
    if (success) {
        client.send(std::string("hello"));
    }
});
```

### HTTP Server

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

server.registerStaticFileHandler("/static", ".");
server.setMaxRequestSize(2 * 1024 * 1024);
server.setStaticFileCacheLimit(8 * 1024 * 1024);
server.start(8080);
```

Reference: [../../examples/http_static_server.cpp](../../examples/http_static_server.cpp)

### HTTP Client

```cpp
FastNet::HttpClient client(FastNet::getGlobalIoService());
client.setConnectTimeout(5000);
client.setRequestTimeout(5000);
client.setReadTimeout(5000);
client.setFollowRedirects(true);
client.setMaxRedirects(5);

client.connect("http://127.0.0.1:8080", [&](bool success, const std::string&) {
    if (!success) {
        return;
    }

    client.get("/healthz", {}, [](const FastNet::HttpResponse& response) {
        // Read statusCode / headers / body
    });
});
```

Reference: [../../examples/http_get_client.cpp](../../examples/http_get_client.cpp)

### WebSocket Server

```cpp
FastNet::WebSocketServer server(FastNet::getGlobalIoService());

server.setMessageCallback([&server](FastNet::ConnectionId id, const std::string& text) {
    server.sendTextToClient(id, text);
});

server.setBinaryCallback([&server](FastNet::ConnectionId id, const FastNet::Buffer& data) {
    server.sendBinaryToClient(id, data);
});

server.setPingInterval(30000);
server.start(8081);
```

Reference: [../../examples/websocket_echo_server.cpp](../../examples/websocket_echo_server.cpp)

### WebSocket Client

```cpp
FastNet::WebSocketClient client(FastNet::getGlobalIoService());
client.setConnectTimeout(5000);
client.setPingInterval(30000);

client.setMessageCallback([](const std::string& text) {
    // Process text message
});

client.setOwnedBinaryCallback([](FastNet::Buffer&& data) {
    // Process binary message
});

client.connect("ws://127.0.0.1:8081", [&](bool success, const std::string&) {
    if (success) {
        client.sendText("hello");
    }
});
```

### UDP

```cpp
FastNet::UdpSocket socket(FastNet::getGlobalIoService());
socket.setBroadcast(true);
socket.setDataReceivedCallback([](const FastNet::Address& sender, const FastNet::Buffer& data) {
    // Process UDP packet
});

socket.bind(9001);
socket.startReceive();
socket.sendTo(FastNet::Address("127.0.0.1", 9001), "ping");
```

## 6. Common Combinations

### Internal Binary RPC
- **Recommended**: `TcpServer`, `TcpClient`, `TcpConnectionPool`, optional `SSLConfig`.
- **Rationale**:
  - Private protocols bypass HTTP/WebSocket fixed overhead.
  - `Buffer&&` paths facilitate low-copy forwarding.
  - Easier control over message boundaries, serialization, and backpressure.

### External HTTP API
- **Recommended**: `HttpServer`, `/healthz`, `registerStaticFileHandler()`, `SSLConfig`, `AsyncLogger`, `PerformanceMonitor`.
- **Notes**:
  - Configure `setMaxRequestSize()`.
  - Enable peer verification for production HTTPS.
  - Use file-backed responses for large static files.

### Real-time Push (WebSocket)
- **Recommended**: `HttpServer` handles login, auth, config, and health checks; `WebSocketServer` handles subscriptions and pushing. Sharing the same `IoService` on different ports, typically exposed under a single entrypoint using a reverse proxy.

### Multi-backend Access
- **Recommended layering**:
  - `ConnectionManager`: Chooses the backend instance.
  - `TcpConnectionPool`: Manages connection reuse inside the selected backend.
  - Do not treat `ConnectionManager` as an actual socket pool, and do not make `TcpConnectionPool` manage multi-backend routing policies.

## 7. Timeout Recommendations

- `connect timeout`: `3s ~ 10s`.
- `read timeout`: `0` for persistent connections (managed by protocol heartbeats).
- `write timeout`: Recommended for high-load environments.
- `WebSocket ping`: `15s ~ 30s`.
- `HTTP request timeout`: Set according to service SLA.

In connection burst scenarios, WebSocket/WSS handshake processes are longer; set timeouts wider than TCP/HTTP.

## 8. Known Boundaries

- `HttpClient` is an HTTP/1.1 keep-alive client, not an HTTP/2 multiplexed client.
- `HttpServer` and `WebSocketServer` are distinct objects; same-port HTTP upgrade routing is not assumed by default.
- `TLS + target-qps` loopback benchmark is excluded from the default matrix.
- Local loopback benchmarks do not replace real multi-machine physical NIC load testing.

## 9. Recommended Reading Order

1. [../../README.md](../../README.md)
2. [COOKBOOK.md](COOKBOOK.md)
3. [API_REFERENCE.md](API_REFERENCE.md)
4. [VALIDATION_CHECKLIST.md](VALIDATION_CHECKLIST.md)
5. [RELEASE_STATUS.md](RELEASE_STATUS.md)
