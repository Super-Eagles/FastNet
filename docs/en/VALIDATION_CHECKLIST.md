# FastNet Validation Checklist

This checklist is used to advance FastNet from "code builds" to "deliverable in the target environment". For the current validation status, see [RELEASE_STATUS.md](RELEASE_STATUS.md).

## 1. Build Verification

Windows:

```powershell
build.bat --clean
build.bat --clean --ssl
build.bat --release-only
build.bat --debug-only
build.bat --no-examples --no-tests
```

Linux:

```bash
chmod +x build.sh
./build.sh --clean
./build.sh --clean --ssl --release-only --test
./build.sh --release-only --no-werror
./build.sh --no-examples --no-tests
```

Key checks:

- Both `FASTNET_ENABLE_SSL=OFF` and `ON` configure successfully.
- When `FASTNET_ENABLE_SSL=ON`, OpenSSL headers and libraries are found by CMake.
- `examples/` and `test/` can be added or skipped using switch options.
- Windows and Linux do not reuse the same build directory.
- Output locations are as expected: libraries to `lib/`, executables to `bin/`.

## 2. Regression Testing

Current test targets:

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
- `fastnet_test_tls_transport` (SSL builds only)

Execution:

```powershell
ctest -C Release --output-on-failure
```

```bash
ctest --output-on-failure
```

Recommended sequence:

1. Run non-SSL Release tests first.
2. Run SSL Release tests.
3. If concurrency, lifecycle, memory pool, event poller, or TLS codes are modified, run Debug tests additionally.

## 3. Example Integration

TCP:

- Run `fastnet_tcp_echo_server`
- Send multiple messages using `telnet`, `nc`, or a custom client
- Verify connection close, timeout, large payload echo, and close-after-flush

UDP:

- Run `fastnet_udp_loopback_benchmark`
- Verify exit without exceptions under different payloads, clients, and durations

HTTP:

- Run `fastnet_http_static_server 8080 .`
- Access `/`, `/healthz`, `/static/...`, `/api/echo`
- Verify `ETag`, `Last-Modified`, `Range`, `304`, and HEAD requests

WebSocket:

- Run `fastnet_websocket_echo_server 8081`
- Connect to `ws://127.0.0.1:8081` using a browser, `websocat`, or a custom client
- Verify text messages, binary messages, ping/pong, and close handshake

TLS:

- Verify server mode using a self-signed certificate first.
- Verify client mode using a local CA or a trusted certificate.
- Cover load failure of certificate chains, mismatched private keys, hostname validation failure, and IPv4/IPv6 host validation.

## 4. Benchmark Verification

Run the unified matrix first:

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

Run specific benchmarks when targeted deep analysis is needed:

- `fastnet_tcp_loopback_benchmark`
- `fastnet_udp_loopback_benchmark`
- `fastnet_http_loopback_benchmark`
- `fastnet_websocket_loopback_benchmark`
- `fastnet_tcp_connect_burst_benchmark`
- `fastnet_http_connect_burst_benchmark`
- `fastnet_websocket_connect_burst_benchmark`

Metrics to record:

- offered / throughput / completion
- Average RTT, P50, P95, P99
- Number of active connections
- Write queue backlog
- CPU, memory, context switching
- Lock contention and system call hotspots

Notes:

- `--loopback-target-qps 0` uses the closed-loop RTT model.
- `--loopback-target-qps > 0` adds plain target-QPS cases.
- Effective in-flight per connection for `HTTP/HTTPS` is currently `1`.
- `TLS + target-qps` loopback is not included in the default matrix.
- If sequential jitter occurs in high-pressure matrices, increase `--case-cooldown-ms`.

## 5. Pre-delivery Checks

- README build commands match the actual scripts.
- Both `build.bat` and `build.sh` print clear errors.
- Packages installed can be consumed via `find_package(FastNet REQUIRED)`.
- Library names, runtime library search paths, and OpenSSL dependencies are documented for Windows and Linux.
- Release status documents benchmark scopes, known boundaries, and incomplete validations.
