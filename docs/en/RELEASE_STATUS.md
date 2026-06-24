# FastNet Release Status

This document records the validations currently completed in the repository, the currently reproducible entry points, and boundaries that still need to be addressed. It is not a performance guarantee; benchmark results are only valid for the specific machine, build configuration, and parameter combination at the time of testing.

## 1. Build Status

- **Windows + Visual Studio + Release + SSL**: Verified locally.
- **Default Non-SSL Build**: Covered by the same set of CMake options, but still needs to be run in the target environment before release.
- **Linux**: `build.sh` and CMake paths are provided, but the Linux real-machine benchmark matrix is not yet recorded as complete.

Common entry points:

```powershell
build.bat --clean --ssl --release-only
```

```bash
./build.sh --clean --ssl --release-only --test
```

## 2. Current Test Targets

The regression test targets currently registered in CMake are as follows:

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
- `fastnet_test_tls_transport` (Only built when `FASTNET_ENABLE_SSL=ON`)

Validation criteria:

- Non-SSL builds must cover all tests except `fastnet_test_tls_transport`.
- SSL builds must additionally cover the TLS transport test.
- After modifying the network core, event poller, connection lifecycle, TLS, or HTTP/WebSocket state machines, the full test suite must be run.

## 3. Benchmark Matrix Records

Windows local plain+TLS matrix records have been verified. With the deep core refactoring on 2026-04-25 (TTAS spinlocks, MpscQueue false-sharing fixes, FlatHashMap lock-free replacement, etc.) and the high-availability/extreme-performance secondary refactoring on 2026-06-21 (comprising full Rule of 5 memory safety rewrite for FlatHashMap, lock-free node allocation refactoring for MpscQueue to eliminate spinlock contention, and adaptive busy-spin polling for IoService to drop idle CPU usage to 0%), the latest peak single-node throughput benchmarks (loopback, 64 concurrency) have reached new highs:

| Protocol | Peak Throughput (Local Loopback) | Average Latency | Remarks |
| --- | --- | --- | --- |
| **UDP** | `~ 17,112.00 QTPS` | N/A | Connectionless, fully benefits from atomic concurrent updates |
| **TCP** | `~ 10,610.00 QTPS` | `~ 6.03 ms` | O(1) state counter and direct PendingSend without polymorphism |
| **WebSocket** | `~ 5,390.00 QTPS` | `~ 5.93 ms` | Includes Mask unpacking, latency is on par with raw TCP |
| **HTTP** | `~ 2,100.00 QTPS` | `~ 15.34 ms` | Bears the full overhead of HTTP 1.0/1.1 Header newline validation |

Long-term stability baseline (Matrix Runner):

| Profile | Status | Reference Time | Report Path |
| --- | --- | --- | --- |
| `smoke` | PASS | ~ `37s` | `tmp/benchmark-matrix-110039050108600/benchmark-matrix-report.md` |
| `standard` | PASS | ~ `3m 23s` | `tmp/benchmark-matrix-108118998147000/benchmark-matrix-report.md` |
| `high` | PASS | ~ `3m 15s` | `tmp/benchmark-matrix-108327364716000/benchmark-matrix-report.md` |
| `soak` | PASS | ~ `15m 12s` | `tmp/benchmark-matrix-111555538535200/benchmark-matrix-report.md` |

Note: `soak` is a long-term stability profile and should not be expected to complete as quickly as `smoke`.

## 4. Benchmark Default Scope

Loopback matrix covers:
- Closed-loop RTT model
- Plain target-QPS model

Connect-burst matrix covers:
- Plain connection burst
- TLS connection burst

Key report fields:
- `Mean Offered`: Actual sending rate
- `Mean Throughput`: Completed throughput
- `Mean Completion`: Completion rate
- `Mean Avg RTT`
- `Mean P95 RTT`
- `Mean P99 RTT`

## 5. Known Boundaries

- `TLS + target-qps` loopback benchmark is currently not included in the default matrix.
- `HTTP/HTTPS` loopback uses HTTP/1.1 keep-alive semantics, where the effective in-flight request per connection is `1`.
- The default connection timeout for `WebSocket/WSS connect-burst` is set wider than TCP/HTTP to accommodate tail latencies during high-concurrency handshakes.
- Current records are mainly for Windows local loopback; Linux, multi-machine real NIC, and long-term production parameter matrices still need separate verification.

## 6. Pre-release Recommendations

Before releasing, at least complete the following:

1. Run `./build.sh --clean --ssl --release-only --test` on a physical Linux machine.
2. Run the `smoke / high / soak` benchmark matrices on a physical Linux machine.
3. Test compatibility with real clients for HTTP, WebSocket, and TLS.
4. Perform multi-machine load testing for the target business parameters, and record CPU, memory, context switching, lock contention, and system calls.
