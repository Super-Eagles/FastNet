# FastNet 发布状态

本文记录当前仓库已经完成的验证、当前可复现入口，以及仍需补齐的边界。它不是性能承诺；benchmark 结果只对当时机器、构建配置和参数组合有效。

## 1. 构建状态

- Windows + Visual Studio + Release + SSL：已通过本机验证。
- 默认非 SSL 构建：由同一套 CMake 选项覆盖，仍需要在发布前按目标环境重跑。
- Linux：已提供 `build.sh` 和 CMake 路径，Linux 实机同口径矩阵尚未记录为完成。

常用入口：

```powershell
build.bat --clean --ssl --release-only
```

```bash
./build.sh --clean --ssl --release-only --test
```

## 2. 当前测试目标

CMake 当前注册的测试目标如下：

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
- `fastnet_test_tls_transport`，仅在 `FASTNET_ENABLE_SSL=ON` 时构建

验证口径：

- 非 SSL 构建应覆盖除 `fastnet_test_tls_transport` 之外的测试。
- SSL 构建应额外覆盖 TLS transport 测试。
- 改动网络核心、事件轮询、连接生命周期、TLS、HTTP/WebSocket 状态机后，必须重跑完整测试集。

## 3. Benchmark Matrix 记录

已有 Windows 本机 plain+TLS 矩阵记录。随着 2026-04-25 的深度核心重构（TTAS 自旋锁、MpscQueue false-sharing 修复、FlatHashMap 无锁化替换等 P0/P1 重构），最新的极限单点吞吐基准（环回测试/64 并发）再次得到突破：

| 测试协议 | 单机压测吞吐量极值 | 平均延迟 | 备注 |
| --- | --- | --- | --- |
| **UDP** | `~ 17,112.00 QTPS` | N/A | 无连接、完全吃尽底层原子并发更新红利 |
| **TCP** | `~ 10,610.00 QTPS` | `~ 6.03 ms` | O(1) 状态计数器及 PendingSend 无多态发送 |
| **WebSocket** | `~ 5,390.00 QTPS` | `~ 5.93 ms` | 包含 Mask 拆包处理，延迟持平原生 TCP |
| **HTTP** | `~ 2,100.00 QTPS` | `~ 15.34 ms` | 完全承载 1.0/1.1 Header 换行强检验开销 |

原有长时稳定性基准（Matrix Runner）：

| Profile | 状态 | 参考耗时 | 报告 |
| --- | --- | --- | --- |
| `smoke` | PASS | 约 `37s` | `tmp/benchmark-matrix-110039050108600/benchmark-matrix-report.md` |
| `standard` | PASS | 约 `3m 23s` | `tmp/benchmark-matrix-108118998147000/benchmark-matrix-report.md` |
| `high` | PASS | 约 `3m 15s` | `tmp/benchmark-matrix-108327364716000/benchmark-matrix-report.md` |
| `soak` | PASS | 约 `15m 12s` | `tmp/benchmark-matrix-111555538535200/benchmark-matrix-report.md` |

`soak` 是长时稳定性档，不应按 smoke 的耗时预期等待。

## 4. Benchmark 默认口径

loopback 矩阵覆盖：

- 闭环 RTT 模型
- plain target-QPS 模型

connect-burst 矩阵覆盖：

- plain 建连 burst
- TLS 建连 burst

报告关键字段：

- `Mean Offered`: 实际发射速率
- `Mean Throughput`: 完成吞吐
- `Mean Completion`: 完成率
- `Mean Avg RTT`
- `Mean P95 RTT`
- `Mean P99 RTT`

## 5. 已知边界

- `TLS + target-qps` loopback benchmark 当前不纳入默认矩阵。
- `HTTP/HTTPS` loopback 是 HTTP/1.1 keep-alive 语义，单连接有效 in-flight 为 `1`。
- `WebSocket/WSS connect-burst` 的默认连接超时比 TCP/HTTP 更宽，用于覆盖高并发握手尾部延迟。
- 当前记录主要是 Windows 本机 loopback；Linux、跨机真实网卡、长时生产参数矩阵仍需单独验证。

## 6. 发布前建议

发布前至少补齐：

1. 在 Linux 实机运行 `./build.sh --clean --ssl --release-only --test`。
2. 在 Linux 实机补跑 `smoke / high / soak` 同口径矩阵。
3. 用真实客户端联调 HTTP、WebSocket、TLS。
4. 对目标业务参数做跨机压测，并记录 CPU、内存、上下文切换、锁竞争和系统调用情况。
