# FastNet 用户指南

这份文档面向实际接入 FastNet 的使用者，重点回答：该选哪个模块、最小代码怎么写、生产配置要注意什么。

如果你已经明确要做什么，优先看 [COOKBOOK.md](COOKBOOK.md)。如果要查公开方法，使用 [API_REFERENCE.md](API_REFERENCE.md)。

## 1. 模块选择

| 需求 | 推荐模块 | 典型用途 |
| --- | --- | --- |
| 原始二进制长连接 | `TcpServer` / `TcpClient` | 私有协议、内网 RPC、代理转发 |
| 无连接小消息 | `UdpSocket` | 广播、探活、状态上报 |
| HTTP 服务 | `HttpServer` | REST API、静态文件、健康检查 |
| HTTP 客户端 | `HttpClient` | 调第三方 API、内部 HTTP 服务 |
| 长连接推送 | `WebSocketServer` / `WebSocketClient` | 聊天、订阅、实时消息 |
| 固定上游复用 | `TcpConnectionPool` | 降低重复建连成本 |
| 多后端调度 | `ConnectionManager` | 客户端侧负载均衡、熔断和健康状态 |

快速判断：

- 自定义协议并追求低开销：优先 TCP。
- 要对接浏览器、curl、Nginx 或 API Gateway：优先 HTTP / WebSocket。
- 只需要发一个包且允许丢包：优先 UDP。
- 同一上游频繁建连：加 `TcpConnectionPool`。
- 多个后端实例要选路：加 `ConnectionManager`。

## 2. 生命周期

大多数对象围绕同一个 `IoService` 工作。推荐模式：

1. 进程启动时调用 `FastNet::initialize()`。
2. 用 `FastNet::getGlobalIoService()` 获取全局 `IoService`。
3. 在同一个 `IoService` 上创建 server、client、pool 或 manager。
4. 停止所有对象后调用 `FastNet::cleanup()`。

最小骨架：

```cpp
#include "FastNet/FastNet.h"

int main() {
    if (FastNet::initialize() != FastNet::ErrorCode::Success) {
        return 1;
    }

    auto& ioService = FastNet::getGlobalIoService();

    // 创建 TcpServer / HttpServer / WebSocketServer / TcpClient 等对象。

    FastNet::cleanup();
    return 0;
}
```

## 3. 基础类型

### `Address`

```cpp
FastNet::Address a1("127.0.0.1", 9000);
FastNet::Address a2("localhost", 8080);
FastNet::Address a3("::1", 8081);
```

常用方法：

- `toString()`
- `isValid()`
- `isIPv6()`
- `isLoopback()`
- `isAnyAddress()`
- `Address::parse(endpoint, defaultPort)`

### `Buffer`

`Buffer` 是 `std::vector<uint8_t>` 别名，适合 TCP、UDP、WebSocket 二进制帧。

推荐选择：

- 文本协议：`std::string` / `std::string_view`
- 二进制协议：`Buffer`
- 转发或广播：优先 move/shared 接口，减少额外复制

### `SSLConfig`

服务端最小配置：

```cpp
FastNet::SSLConfig ssl;
ssl.enableSSL = true;
ssl.certificateFile = "server.crt";
ssl.privateKeyFile = "server.key";
ssl.verifyPeer = false;
```

客户端生产配置：

```cpp
FastNet::SSLConfig ssl;
ssl.enableSSL = true;
ssl.caFile = "ca.crt";
ssl.verifyPeer = true;
ssl.hostnameVerification = "api.example.com";
```

注意：TLS 相关代码需要用 `FASTNET_ENABLE_SSL=ON` 构建。

## 4. 回调和数据所有权

FastNet 的收包回调通常有三类：

- `const Buffer&`: 立刻解析并消费。
- `Buffer&&`: 收到后直接转发或移动到业务队列。
- `shared_ptr<const Buffer>`: 一份数据要分发给多个下游。

建议：

- Echo / proxy：优先 `setOwnedDataReceivedCallback()`。
- 广播：优先 shared 路径。
- HTTP/WebSocket 文本业务：让上层处理字符串或结构体，不要无意义复制底层 buffer。

## 5. 最小用法

### TCP 服务端

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

参考：[../examples/tcp_echo_server.cpp](../examples/tcp_echo_server.cpp)

### TCP 客户端

```cpp
FastNet::TcpClient client(FastNet::getGlobalIoService());
client.setConnectTimeout(5000);
client.setReadTimeout(0);
client.setWriteTimeout(0);

client.setSharedDataReceivedCallback([](const std::shared_ptr<const FastNet::Buffer>& data) {
    // 解析或转发 data
});

client.setErrorCallback([](FastNet::ErrorCode, const std::string& message) {
    // 记录错误
});

client.connect("127.0.0.1", 9000, [&](bool success, const std::string&) {
    if (success) {
        client.send(std::string("hello"));
    }
});
```

### HTTP 服务端

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

参考：[../examples/http_static_server.cpp](../examples/http_static_server.cpp)

### HTTP 客户端

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
        // 读取 statusCode / headers / body
    });
});
```

参考：[../examples/http_get_client.cpp](../examples/http_get_client.cpp)

### WebSocket 服务端

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

参考：[../examples/websocket_echo_server.cpp](../examples/websocket_echo_server.cpp)

### WebSocket 客户端

```cpp
FastNet::WebSocketClient client(FastNet::getGlobalIoService());
client.setConnectTimeout(5000);
client.setPingInterval(30000);

client.setMessageCallback([](const std::string& text) {
    // 处理文本消息
});

client.setOwnedBinaryCallback([](FastNet::Buffer&& data) {
    // 处理二进制消息
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
    // 处理 UDP 包
});

socket.bind(9001);
socket.startReceive();
socket.sendTo(FastNet::Address("127.0.0.1", 9001), "ping");
```

## 6. 常见组合

### 内网二进制 RPC

推荐：`TcpServer`、`TcpClient`、`TcpConnectionPool`、可选 `SSLConfig`。

原因：

- 私有协议避免 HTTP/WebSocket 固定开销。
- `Buffer&&` 路径适合低复制转发。
- 更容易控制消息边界、序列化和回压策略。

### 对外 HTTP API

推荐：`HttpServer`、`/healthz`、`registerStaticFileHandler()`、`SSLConfig`、`AsyncLogger`、`PerformanceMonitor`。

注意：

- 设置 `setMaxRequestSize()`。
- 生产 HTTPS 打开 peer verification。
- 静态大文件优先 file-backed response。

### 实时推送

推荐：`HttpServer` 负责登录、鉴权、配置和健康检查；`WebSocketServer` 负责订阅与推送。两者共享同一个 `IoService`，通常使用不同端口，再由反向代理统一入口。

### 多后端访问

推荐分层：

- `ConnectionManager`: 选哪个后端。
- `TcpConnectionPool`: 选中后端内部复用 TCP 连接。

不要把 `ConnectionManager` 当成实际 socket 池，也不要让 `TcpConnectionPool` 负责多后端策略。

## 7. 超时建议

- `connect timeout`: 通常 `3s ~ 10s`。
- `read timeout`: 长连接常设 `0`，由协议心跳控制。
- `write timeout`: 高负载场景建议开启。
- `WebSocket ping`: 常用 `15s ~ 30s`。
- `HTTP request timeout`: 按接口 SLA 设定。

建连 burst 场景中，WebSocket/WSS 的握手链路更长，超时应比 TCP/HTTP 更宽。

## 8. 已知边界

- `HttpClient` 当前是 HTTP/1.1 keep-alive 客户端，不是 HTTP/2 多路复用客户端。
- `HttpServer` 和 `WebSocketServer` 当前是两个独立服务对象；不建议默认假设同端口 HTTP upgrade 路由。
- `TLS + target-qps` loopback benchmark 不纳入默认矩阵。
- 本机 loopback benchmark 不能替代跨机真实网卡压测。

## 9. 推荐阅读顺序

1. [../README.md](../README.md)
2. [COOKBOOK.md](COOKBOOK.md)
3. [API_REFERENCE.md](API_REFERENCE.md)
4. [VALIDATION_CHECKLIST.md](VALIDATION_CHECKLIST.md)
5. [RELEASE_STATUS.md](RELEASE_STATUS.md)
