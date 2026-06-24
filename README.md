# FastNet

[![CI](https://github.com/Super-Eagles/FastNet/actions/workflows/ci.yml/badge.svg)](https://github.com/Super-Eagles/FastNet/actions/workflows/ci.yml)
[![License](https://img.shields.io/github/license/Super-Eagles/FastNet)](LICENSE)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://en.cppreference.com/w/cpp/17)
[![Version](https://img.shields.io/badge/version-1.4.0-green.svg)](CMakeLists.txt)

[中文版](README中文.md)

A production-grade, cross-platform C++17 networking library covering **TCP, UDP, HTTP/1.1, WebSocket, TLS, connection pooling, multi-backend management, logging, and performance monitoring** — all built on a unified async I/O engine.

---

## Features

| Layer | Capabilities |
|---|---|
| **Transport** | TCP server/client, UDP send/receive, TLS via OpenSSL |
| **Protocol** | HTTP/1.1 server & client, WebSocket server & client (text/binary/ping-pong/close handshake) |
| **Connection Management** | `TcpConnectionPool` for upstream connection reuse; `ConnectionManager` for multi-backend routing and health tracking |
| **Async Runtime** | `IoService` thread pool, `EventPoller` (epoll/IOCP), `TimerManager` |
| **Observability** | Async file logger, `PerformanceMonitor`, rich `Error` model with source location |
| **Cross-platform** | Windows (IOCP) + Linux (epoll), same CMake build |

---

## Performance (loopback, single machine)

> Measured on Windows with Release + MSVC. Results are loopback baselines; cross-machine numbers will differ.

| Protocol | Peak Throughput | Avg Latency |
|---|---|---|
| UDP | ~17,112 QTPS | — |
| TCP | ~10,610 QTPS | ~6.03 ms |
| WebSocket | ~5,390 QTPS | ~5.93 ms |
| HTTP/1.1 | ~2,100 QTPS | ~15.34 ms |

---

## Quick Start

### Minimal TCP Echo Server

```cpp
#include "FastNet/FastNet.h"
#include <iostream>

int main() {
    if (FastNet::initialize() != FastNet::ErrorCode::Success)
        return 1;

    auto& io = FastNet::getGlobalIoService();
    FastNet::TcpServer server(io);

    server.setOwnedDataReceivedCallback([&server](FastNet::ConnectionId id, FastNet::Buffer&& data) {
        server.sendToClient(id, std::move(data));   // echo back
    });

    server.start(9000);

    std::string line;
    std::getline(std::cin, line);   // press Enter to stop

    server.stop();
    FastNet::cleanup();
}
```

### HTTP Server

```cpp
FastNet::HttpServer server(FastNet::getGlobalIoService());

server.registerGet("/healthz", [](const FastNet::HttpRequest&, FastNet::HttpResponse& res) {
    res.statusCode = 200;
    res.body = "ok";
});

server.registerStaticFileHandler("/static", ".");
server.start(8080);
```

### WebSocket Server

```cpp
FastNet::WebSocketServer ws(FastNet::getGlobalIoService());

ws.setMessageCallback([&ws](FastNet::ConnectionId id, const std::string& text) {
    ws.sendTextToClient(id, text);  // echo
});

ws.setPingInterval(30000);
ws.start(8081);
```

### TLS Client

```cpp
FastNet::SSLConfig ssl;
ssl.enableSSL         = true;
ssl.caFile            = "ca.crt";
ssl.verifyPeer        = true;
ssl.hostnameVerification = "api.example.com";

FastNet::TcpClient client(FastNet::getGlobalIoService());
client.connect("api.example.com", 443, [](bool ok, const std::string&) {}, ssl);
```

---

## Building

### Prerequisites

| Platform | Toolchain |
|---|---|
| Windows | Visual Studio 2019/2022 with C++ workload, CMake ≥ 3.15 |
| Linux | GCC ≥ 9 or Clang ≥ 10, CMake ≥ 3.15, optional Ninja |

Optional: OpenSSL (for TLS support).

### Windows

```powershell
build.bat --clean --release-only
build.bat --clean --ssl --release-only   # with TLS
```

### Linux

```bash
sudo apt install build-essential cmake ninja-build libssl-dev
./build.sh --clean --release-only --test
./build.sh --clean --ssl --release-only --test   # with TLS
```

### CMake directly

```bash
# Linux
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DFASTNET_ENABLE_SSL=OFF
cmake --build build --parallel

# Windows
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DFASTNET_ENABLE_SSL=OFF
cmake --build build --config Release
```

Key CMake options:

| Option | Default | Description |
|---|---|---|
| `FASTNET_BUILD_EXAMPLES` | ON | Build example programs |
| `FASTNET_BUILD_TESTS` | ON | Build regression tests |
| `FASTNET_ENABLE_SSL` | OFF | Enable OpenSSL/TLS |
| `FASTNET_WARNINGS_AS_ERRORS` | ON | Treat warnings as errors |
| `FASTNET_INSTALL_CMAKE_PACKAGE` | ON | Install CMake package files |
| `FASTNET_BUILD_STATIC` | OFF | Build FastNet as a static library |

### Consuming via CMake `find_package`

```cmake
find_package(FastNet REQUIRED)
target_link_libraries(my_app PRIVATE FastNet::FastNet)
```

---

## Running Tests

```bash
# Linux
./build.sh --clean --release-only --test

# Windows
build.bat --clean --release-only
cd build && ctest -C Release --output-on-failure
```

---

## Examples

Binaries are output to `bin/`. Start with:

```bash
./bin/fastnet_tcp_echo_server
./bin/fastnet_http_static_server 8080 .
./bin/fastnet_websocket_echo_server 8081
./bin/fastnet_benchmark_matrix_runner   # runs the full benchmark matrix
```

Full list of examples in [`examples/`](examples/).

---

## Documentation

| Document | Contents |
|---|---|
| [docs/en/USER_GUIDE.md](docs/en/USER_GUIDE.md) | Module selection, lifecycle, minimal usage patterns |
| [docs/en/COOKBOOK.md](docs/en/COOKBOOK.md) | Task-oriented recipes |
| [docs/en/API_REFERENCE.md](docs/en/API_REFERENCE.md) | Public classes and methods |
| [docs/en/VALIDATION_CHECKLIST.md](docs/en/VALIDATION_CHECKLIST.md) | Build, test, and benchmark checklist |
| [docs/en/RELEASE_STATUS.md](docs/en/RELEASE_STATUS.md) | Verified configurations and known boundaries |

---

## Project Structure

```
include/FastNet/   — Public headers
src/               — Core implementation
examples/          — Example programs and benchmarks
test/              — Regression tests (20 files)
docs/              — Documentation
cmake/             — CMake package config template
```

---

## License

[LGPL-2.1](LICENSE)
