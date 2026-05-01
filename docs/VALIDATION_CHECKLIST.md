# FastNet 验证清单

这份清单用于把 FastNet 从“代码能构建”推进到“目标环境可交付”。当前验证状态见 [RELEASE_STATUS.md](RELEASE_STATUS.md)。

## 1. 构建验证

Windows：

```powershell
build.bat --clean
build.bat --clean --ssl
build.bat --release-only
build.bat --debug-only
build.bat --no-examples --no-tests
```

Linux：

```bash
chmod +x build.sh
./build.sh --clean
./build.sh --clean --ssl --release-only --test
./build.sh --release-only --no-werror
./build.sh --no-examples --no-tests
```

重点检查：

- `FASTNET_ENABLE_SSL=OFF` 和 `ON` 都能配置成功。
- `FASTNET_ENABLE_SSL=ON` 时 OpenSSL 头文件和库能被 CMake 找到。
- `examples/` 和 `test/` 能按选项开关加入或跳过。
- Windows 和 Linux 不复用同一个构建目录。
- 产物位置符合预期：库到 `lib/`，可执行文件到 `bin/`。

## 2. 回归测试

当前测试目标：

- `fastnet_test_base64`
- `fastnet_test_configuration`
- `fastnet_test_connection_manager`
- `fastnet_test_error_model`
- `fastnet_test_http_loopback`
- `fastnet_test_http_parser`
- `fastnet_test_network_core`
- `fastnet_test_runtime_components`
- `fastnet_test_tcp_transport`
- `fastnet_test_udp_socket`
- `fastnet_test_websocket_loopback`
- `fastnet_test_websocket_protocol`
- `fastnet_test_tls_transport`，仅 SSL 构建包含

执行方式：

```powershell
ctest -C Release --output-on-failure
```

```bash
ctest --output-on-failure
```

建议顺序：

1. 先跑非 SSL Release 测试。
2. 再跑 SSL Release 测试。
3. 改了并发、生命周期、内存池、事件轮询或 TLS 后，再补 Debug 测试。

## 3. 示例联调

TCP：

- 运行 `fastnet_tcp_echo_server`
- 用 `telnet`、`nc` 或自写 client 发送多条消息
- 验证连接关闭、超时、大包回显和 close-after-flush

UDP：

- 运行 `fastnet_udp_loopback_benchmark`
- 验证不同 payload、clients、duration 下无异常退出

HTTP：

- 运行 `fastnet_http_static_server 8080 .`
- 访问 `/`、`/healthz`、`/static/...`、`/api/echo`
- 验证 `ETag`、`Last-Modified`、`Range`、`304`、HEAD 请求

WebSocket：

- 运行 `fastnet_websocket_echo_server 8081`
- 用浏览器、`websocat` 或自写 client 连接 `ws://127.0.0.1:8081`
- 验证文本消息、二进制消息、ping/pong、close handshake

TLS：

- 用自签名证书先验证 server mode。
- 用本地 CA 或受信任证书验证 client mode。
- 覆盖证书链加载失败、私钥不匹配、主机名校验失败、IPv4/IPv6 主机校验。

## 4. Benchmark 验证

优先跑统一矩阵：

```powershell
bin\fastnet_benchmark_matrix_runner --profile smoke --ssl
bin\fastnet_benchmark_matrix_runner --profile high --ssl
bin\fastnet_benchmark_matrix_runner --profile soak --ssl
bin\fastnet_benchmark_matrix_runner --profile high --ssl --loopback-max-inflight 8 --loopback-target-qps 40000
```

```bash
./bin/fastnet_benchmark_matrix_runner --profile smoke --ssl
./bin/fastnet_benchmark_matrix_runner --profile high --ssl
./bin/fastnet_benchmark_matrix_runner --profile soak --ssl
./bin/fastnet_benchmark_matrix_runner --profile high --ssl --loopback-max-inflight 8 --loopback-target-qps 40000
```

需要定向深挖时，再单独跑：

- `fastnet_tcp_loopback_benchmark`
- `fastnet_udp_loopback_benchmark`
- `fastnet_http_loopback_benchmark`
- `fastnet_websocket_loopback_benchmark`
- `fastnet_tcp_connect_burst_benchmark`
- `fastnet_http_connect_burst_benchmark`
- `fastnet_websocket_connect_burst_benchmark`

记录指标：

- offered / throughput / completion
- 平均 RTT、P50、P95、P99
- 活跃连接数
- 写队列积压
- CPU、内存、上下文切换
- 锁竞争和系统调用热点

说明：

- `--loopback-target-qps 0` 是闭环 RTT 模型。
- `--loopback-target-qps > 0` 会追加 plain target-QPS case。
- `HTTP/HTTPS` 单连接有效 in-flight 当前仍为 `1`。
- `TLS + target-qps` loopback 不纳入默认矩阵。
- 高压矩阵有顺序抖动时，可调大 `--case-cooldown-ms`。

## 5. 交付前检查

- README 的构建命令和实际脚本一致。
- `build.bat` 和 `build.sh` 都能打印清晰错误。
- `find_package(FastNet REQUIRED)` 可消费安装后的包。
- Windows 和 Linux 的库名、运行库搜索路径、OpenSSL 依赖已说明。
- 发布说明明确 benchmark 口径、已知边界和未完成验证。
