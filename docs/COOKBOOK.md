# FastNet Cookbook

Cookbook 按“我要完成什么任务”组织。如果要理解整体设计，先看 [USER_GUIDE.md](USER_GUIDE.md)；如果要查方法名，直接看 [API_REFERENCE.md](API_REFERENCE.md)。

## 1. 快速选型

| 任务 | 推荐组合 | 说明 |
| --- | --- | --- |
| 高性能二进制服务 | `TcpServer` + owned receive callback | 私有协议、RPC、代理转发 |
| 上游 TCP 客户端 | `TcpClient` / `TcpConnectionPool` | 长连接或固定后端复用 |
| HTTP API + 静态文件 | `HttpServer` | REST、健康检查、静态资源 |
| 调 HTTP/HTTPS API | `HttpClient` | 标准请求-响应客户端 |
| 实时推送 | `WebSocketServer` / `WebSocketClient` | 订阅、聊天、双向消息 |
| 广播探活 | `UdpSocket` | 允许丢包的轻量消息 |
| 多后端调度 | `ConnectionManager` + pool/client | 负载均衡、熔断、健康状态 |

## 2. 高性能 TCP Echo / 私有协议服务

适用：

- 内网二进制 RPC
- 网关到后端的私有协议
- 单机高吞吐转发

推荐做法：

1. 一个进程优先共享一个 `IoService`。
2. 收包用 `setOwnedDataReceivedCallback()`。
3. 转发或 echo 直接 `std::move(data)`。
4. 长连接的 read timeout 不要设得过短。

```cpp
FastNet::TcpServer server(FastNet::getGlobalIoService());

server.setOwnedDataReceivedCallback([&server](FastNet::ConnectionId id, FastNet::Buffer&& data) {
    server.sendToClient(id, std::move(data));
});

server.setServerErrorCallback([](const FastNet::Error& error) {
    // 记录 error.toString()
});

server.start(9000);
```

可继续加：

- `setMaxConnections()`
- `setConnectionTimeout()`
- `setWriteTimeout()`
- `closeClientAfterPendingWrites()`

参考：[../examples/tcp_echo_server.cpp](../examples/tcp_echo_server.cpp)

## 3. 上游 TCP 客户端或代理连接器

适用：

- 主动连接后端的 RPC client
- TCP 代理
- 长连接上游

```cpp
FastNet::TcpClient client(FastNet::getGlobalIoService());
client.setConnectTimeout(5000);
client.setSharedDataReceivedCallback([](const std::shared_ptr<const FastNet::Buffer>& data) {
    // 解析或转发
});

client.connect("127.0.0.1", 9000, [&](bool success, const std::string&) {
    if (success) {
        client.send(std::string("ping"));
    }
});
```

如果目标上游固定且请求频繁，不要每次新建 `TcpClient`，改用 `TcpConnectionPool`。

## 4. HTTP API + 静态文件

适用：

- 管理后台
- 健康检查
- 简单 REST API
- 内置静态页面

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

建议：

- 明确设置 `Content-Type`。
- 给请求体设上限。
- 静态资源走 `registerStaticFileHandler()`。
- 大文件响应优先 file-backed body。

参考：[../examples/http_static_server.cpp](../examples/http_static_server.cpp)

## 5. 调 HTTP/HTTPS API

适用：

- 调第三方 REST API
- 服务间 HTTP 调用
- 健康检查探针

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
        // 处理响应
    });
});
```

注意：

- `HttpClient` 当前是 HTTP/1.1 keep-alive。
- 单连接有效 in-flight 是 `1`。
- 要提高总吞吐，增加连接数或在业务层做连接池。

参考：[../examples/http_get_client.cpp](../examples/http_get_client.cpp)

## 6. WebSocket 推送服务

适用：

- 聊天室
- 订阅推送
- 实时状态同步

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

建议：

- 文本消息走 `setMessageCallback()`。
- 二进制转发优先 owned callback。
- 连接退出尽量走 close handshake。

参考：[../examples/websocket_echo_server.cpp](../examples/websocket_echo_server.cpp)

## 7. 同进程跑 HTTP 和 WebSocket

当前更稳的做法是同进程、同 `IoService`、不同端口：

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
    // 推送逻辑
});
ws.start(8081);
```

需要统一入口时，把 HTTP 和 WebSocket 交给 Nginx 或网关层反向代理。不要默认假设库内已经内建同端口 upgrade 路由。

## 8. 打开 TLS

服务端：

```cpp
FastNet::SSLConfig ssl;
ssl.enableSSL = true;
ssl.certificateFile = "server.crt";
ssl.privateKeyFile = "server.key";
ssl.verifyPeer = false;

server.start(8443, "0.0.0.0", ssl);
```

客户端：

```cpp
FastNet::SSLConfig ssl;
ssl.enableSSL = true;
ssl.caFile = "ca.crt";
ssl.verifyPeer = true;
ssl.hostnameVerification = "api.example.com";
```

适用入口：

- `TcpServer::start(..., ssl)`
- `TcpClient::connect(..., ssl)`
- `HttpServer::start(..., ssl)`
- `HttpClient::setSSLConfig(ssl)`
- `WebSocketServer::start(..., ssl)`
- `WebSocketClient::setSSLConfig(ssl)`

构建时必须启用：

```powershell
build.bat --ssl --release-only
```

```bash
./build.sh --ssl --release-only
```

## 9. 使用连接池减少重复建连

适用：

- 上游固定
- 请求频繁
- 建连成本明显

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

归还连接时用 `release()`，不要把 `PooledConnection` 长期拿出池外独占。

## 10. 多后端负载均衡和熔断

适用：

- 服务发现后有多个后端实例
- 需要 RoundRobin、LeastConnections、WeightedRoundRobin、IPHash
- 需要基础熔断和健康状态

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

分工：

- `ConnectionManager` 负责选后端和记录状态。
- `TcpConnectionPool` 负责固定后端内部连接复用。

## 11. UDP 广播、探活或轻量消息

```cpp
FastNet::UdpSocket socket(FastNet::getGlobalIoService());
socket.setBroadcast(true);
socket.setDataReceivedCallback([](const FastNet::Address& sender, const FastNet::Buffer& data) {
    // 处理消息
});

socket.bind(9900);
socket.startReceive();
socket.sendTo(FastNet::Address("255.255.255.255", 9900), "ping");
```

UDP 不保证到达、不保证顺序、不提供连接状态。必须可靠投递时，回到 TCP 或 WebSocket。

## 12. 常见问题

### HTTP benchmark 为什么比 TCP 低？

HTTP 额外包含头解析、路由分发、请求边界、响应构造和 keep-alive 管理。它低于原始 TCP 是正常现象。

### `soak` 为什么跑很久？

`soak` 是长时稳定性验证，不是快速 smoke。已有样本约 15 分钟，目标机器可能更久。

### HTTP 和 WebSocket 能不能同端口？

当前不建议默认这么做。推荐同进程同 `IoService`，不同端口，反向代理统一入口。

### 什么时候必须开 TLS？

对外服务默认应该开。内网服务按信任边界和合规要求决定。benchmark 建议 plain 和 TLS 都测，因为开销差异很明显。

## 13. 继续阅读

- [USER_GUIDE.md](USER_GUIDE.md)
- [API_REFERENCE.md](API_REFERENCE.md)
- [VALIDATION_CHECKLIST.md](VALIDATION_CHECKLIST.md)
- [RELEASE_STATUS.md](RELEASE_STATUS.md)
