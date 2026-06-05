# FastNet

FastNet 是一个 C++17 网络库，当前代码已经覆盖 TCP、UDP、HTTP、WebSocket、TLS、连接管理、配置、日志、监控和错误模型。仓库里的重点不只是“能编译”，而是把运行时、协议层和验证路径整理到可以持续回归的状态。

## 项目结构

- `include/FastNet/`: 对外公开头文件
- `src/`: 核心实现
- `examples/`: 示例程序和 benchmark
- `test/`: C++ 回归测试
- `cmake/`: CMake package 配置模板
- `docs/`: 用户指南、API 参考、验证清单和发布状态

## 文档入口

建议按这个顺序看：

1. [docs/USER_GUIDE.md](docs/USER_GUIDE.md): 模块选择、生命周期、最小用法
2. [docs/COOKBOOK.md](docs/COOKBOOK.md): 按任务组织的落地配方
3. [docs/API_REFERENCE.md](docs/API_REFERENCE.md): 公开类和方法速查
4. [docs/VALIDATION_CHECKLIST.md](docs/VALIDATION_CHECKLIST.md): 构建、测试、联调和压测步骤
5. [docs/RELEASE_STATUS.md](docs/RELEASE_STATUS.md): 当前已经完成的验证和保留边界

## 能力概览

- TCP: 服务端、客户端、连接池、close-after-flush、move/shared buffer 路径
- UDP: 无连接收发、广播、loopback benchmark
- HTTP: HTTP/1.1 server/client、静态文件、Range、304、HEAD、重定向客户端
- WebSocket: server/client、文本/二进制消息、ping/pong、close handshake
- TLS: 通过 `FASTNET_ENABLE_SSL=ON` 条件编译接入 OpenSSL
- 运行时: `IoService`、`EventPoller`、`TimerManager`
- 工程能力: CMake 构建、安装导出 `FastNet::FastNet`、Windows/Linux 脚本入口

## 构建

### Windows

需要 Visual Studio 2019/2022 C++ 工具链和 CMake。

```powershell
build.bat --clean
build.bat --ssl --release-only
build.bat --no-examples --no-tests
```

常用参数：

- `--clean`: 删除构建目录后重新配置
- `--ssl`: 打开 OpenSSL/TLS 支持
- `--release-only` / `--debug-only`: 只构建单一配置
- `--no-examples` / `--no-tests`: 跳过示例或测试目标
- `--build-dir DIR`: 指定构建目录
- `--generator GEN`: 指定 CMake generator

### Linux

需要 C++17 编译器、CMake 和可选 Ninja。Ubuntu/Debian 示例：

```bash
sudo apt install build-essential cmake ninja-build
sudo apt install libssl-dev
```

使用仓库脚本：

```bash
chmod +x build.sh
./build.sh --clean --release-only
./build.sh --ssl --release-only --test
./build.sh --no-examples --no-tests
```

Linux 脚本默认使用 `build/linux/Release` 和 `build/linux/Debug`，避免混用 Windows 的 Visual Studio 构建目录。产物仍输出到项目根目录下的 `bin/` 和 `lib/`。

如果目标环境的 GCC/Clang 对 warning 更严格，可以先用：

```bash
./build.sh --release-only --no-werror
```

### 直接使用 CMake

Windows 示例：

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DFASTNET_ENABLE_SSL=OFF
cmake --build build --config Release
```

Linux 示例：

```bash
cmake -S . -B build/linux/Release -G Ninja -DCMAKE_BUILD_TYPE=Release -DFASTNET_ENABLE_SSL=OFF
cmake --build build/linux/Release --parallel
```

主要 CMake 选项：

- `FASTNET_BUILD_EXAMPLES`: 是否构建 `examples/`
- `FASTNET_BUILD_TESTS`: 是否构建 `test/`
- `FASTNET_ENABLE_SSL`: 是否启用 OpenSSL/TLS
- `FASTNET_WARNINGS_AS_ERRORS`: 是否把 warning 当错误
- `FASTNET_INSTALL_CMAKE_PACKAGE`: 是否安装 CMake package 文件

## 示例程序

示例目标默认输出到 `bin/`：

- `fastnet_tcp_echo_server`: TCP Echo 服务端
- `fastnet_tcp_loopback_benchmark`: TCP loopback 吞吐与 RTT 基线
- `fastnet_tcp_connect_burst_benchmark`: TCP 建连 burst 基线
- `fastnet_udp_loopback_benchmark`: UDP loopback 吞吐基线
- `fastnet_http_get_client`: HTTP GET 客户端
- `fastnet_http_static_server`: HTTP 静态文件、健康检查和 echo API
- `fastnet_http_loopback_benchmark`: HTTP/HTTPS loopback 基线
- `fastnet_http_connect_burst_benchmark`: HTTP/HTTPS 建连 burst 基线
- `fastnet_websocket_echo_server`: WebSocket 文本/二进制 echo 服务端
- `fastnet_websocket_loopback_benchmark`: WebSocket/WSS loopback 基线
- `fastnet_websocket_connect_burst_benchmark`: WebSocket/WSS 建连 burst 基线
- `fastnet_benchmark_matrix_runner`: 串行执行 benchmark 矩阵并输出 Markdown 报告

Windows 执行示例：

```powershell
bin\fastnet_http_static_server 8080 .
bin\fastnet_websocket_echo_server 8081
```

Linux 执行示例：

```bash
./bin/fastnet_http_static_server 8080 .
./bin/fastnet_websocket_echo_server 8081
```

