# FastNet API Reference

This document compiles the commonly used APIs by their public headers. It is designed for quick lookups of method names and composition relationships; refer to `include/FastNet/*.h` for complete signatures.

Related Documents:
- [USER_GUIDE.md](USER_GUIDE.md)
- [COOKBOOK.md](COOKBOOK.md)
- [VALIDATION_CHECKLIST.md](VALIDATION_CHECKLIST.md)

## 1. Main Entrypoint

Header: [../../include/FastNet/FastNet.h](../../include/FastNet/FastNet.h)

```cpp
FastNet::ErrorCode initialize(size_t threadCount = 0);
void cleanup();
bool isInitialized();
```

Recommendations:
- Call `initialize()` as early as possible after process startup.
- Most applications use `getGlobalIoService()` to create network objects.
- Call `cleanup()` after all servers and clients have stopped.

## 2. Basic Types

Header: [../../include/FastNet/Config.h](../../include/FastNet/Config.h)

### `Address`

Fields:
- `std::string ip`
- `uint16_t port`

Common Methods:
- `host()`
- `hasPort()`
- `normalizedHost()`
- `isValid()`
- `isIPv6()`
- `isLoopback()`
- `isAnyAddress()`
- `toString()`
- `Address::parse(endpoint, defaultPort)`

Static Validation Methods:
- `isValidIPv4(ip)`
- `isValidIPv6(host)`
- `isValidHostname(host)`
- `isValidHost(host)`
- `isLoopbackHost(host)`
- `isAnyHost(host)`
- `isValidPort(port)`

### `Buffer`

```cpp
using Buffer = std::vector<uint8_t>;
```
Used for TCP, UDP, and WebSocket binary data.

### `SSLConfig`

Fields:
- `enableSSL`
- `certificateFile`
- `privateKeyFile`
- `caFile`
- `verifyPeer`
- `hostnameVerification`

Note: TLS requires compiling with `FASTNET_ENABLE_SSL=ON`.

### `ErrorCode`

Covers socket, connection, bind, listen, resolve, timeout, HTTP, WebSocket, SSL, authentication, compression, and other error categories.

## 3. Runtime and Timers

### `IoService`

Header: [../../include/FastNet/IoService.h](../../include/FastNet/IoService.h)

Core Methods:
- `IoService(size_t threadCount = 0)`
- `start()`
- `stop()`
- `join()`
- `post(const Task&)`
- `post(Task&&)`
- `getPoller()`
- `isRunning()`
- `getThreadCount()`

Global Entrypoints:
- `configureGlobalIoService(threadCount)`
- `getGlobalIoService()`
- `shutdownGlobalIoService()`

### `TimerManager`

Header: [../../include/FastNet/Timer.h](../../include/FastNet/Timer.h)

Core Methods:
- `start()`
- `stop()`
- `isRunning()`
- `addTimer(delay, callback)`
- `addRepeatingTimer(interval, callback)`
- `cancelTimer(timerId)`
- `getActiveTimerCount()`

Global Entrypoints:
- `getGlobalTimerManager()`
- `shutdownGlobalTimerManager()`

### `Timer`

Core Methods:
- `start(delay, callback, repeat = false)`
- `stop()`
- `isRunning()`

### `ConnectionTimeoutManager`

Core Methods:
- `setConnectionTimeout(connId, timeout, callback)`
- `refreshConnection(connId)`
- `removeConnection(connId)`
- `getManagedConnectionCount()`

## 4. TCP

### `TcpServer`

Header: [../../include/FastNet/TcpServer.h](../../include/FastNet/TcpServer.h)

Lifecycle:
- `TcpServer(IoService&)`
- `start(port, bindAddress, sslConfig)`
- `start(Address, sslConfig)`
- `stop()`

Sending:
- `sendToClient(clientId, const Buffer&)`
- `sendToClient(clientId, Buffer&&)`
- `sendToClient(clientId, shared_ptr<const Buffer>)`
- `sendToClient(clientId, string&&)`
- `sendToClient(clientId, string_view)`
- `sendFileToClient(clientId, prefix, filePath, offset, length)`
- `broadcast(...)`

Connection Control:
- `disconnectClient(clientId)`
- `closeClientAfterPendingWrites(clientId)`

Status:
- `getClientCount()`
- `getClientIds()`
- `getClientAddress(clientId)`
- `hasClient(clientId)`
- `getListenAddress()`
- `isRunning()`

Callbacks:
- `setClientConnectedCallback(callback)`
- `setClientDisconnectedCallback(callback)`
- `setDataReceivedCallback(callback)`
- `setOwnedDataReceivedCallback(callback)`
- `setSharedDataReceivedCallback(callback)`
- `setServerErrorCallback(callback)`

Configuration:
- `setConnectionTimeout(timeoutMs)`
- `setReadTimeout(timeoutMs)`
- `setWriteTimeout(timeoutMs)`
- `setMaxConnections(maxConnections)`

### `TcpClient`

Header: [../../include/FastNet/TcpClient.h](../../include/FastNet/TcpClient.h)

Connection:
- `connect(host, port, callback, sslConfig)`
- `connect(Address, callback, sslConfig)`
- `disconnect()`
- `disconnectAfterPendingWrites()`

Sending:
- `send(const Buffer&)`
- `send(Buffer&&)`
- `send(shared_ptr<const Buffer>)`
- `send(string&&)`
- `send(string_view)`
- `send(shared_ptr<const string>)`

Callbacks:
- `setConnectCallback(callback)`
- `setDisconnectCallback(callback)`
- `setDataReceivedCallback(callback)`
- `setOwnedDataReceivedCallback(callback)`
- `setSharedDataReceivedCallback(callback)`
- `setErrorCallback(callback)`

Status:
- `getLocalAddress()`
- `getRemoteAddress()`
- `isConnected()`
- `isSecure()`
- `getPendingWriteBytes()`
- `getLastError()`

Timeouts:
- `setConnectTimeout(timeoutMs)`
- `setReadTimeout(timeoutMs)`
- `setWriteTimeout(timeoutMs)`

## 5. UDP

### `UdpSocket`

Header: [../../include/FastNet/UdpSocket.h](../../include/FastNet/UdpSocket.h)

Lifecycle:
- `bind(port, bindAddress)`
- `bind(Address)`
- `startReceive()`
- `stopReceive()`

Sending:
- `sendTo(Address, const Buffer&)`
- `sendTo(Address, string_view)`

Callbacks and Configuration:
- `setDataReceivedCallback(callback)`
- `setErrorCallback(callback)`
- `setReceiveBufferSize(size)`
- `setSendBufferSize(size)`
- `setBroadcast(enable)`

Status:
- `getLocalAddress()`
- `isBound()`
- `isReceiving()`

## 6. HTTP

### `HttpResponse`

Header: [../../include/FastNet/HttpCommon.h](../../include/FastNet/HttpCommon.h)

Fields:
- `statusCode`
- `statusMessage`
- `headers`
- `body`
- `hasFileBody`
- `filePath`
- `fileOffset`
- `fileLength`

Note: Populate `body` directly for small responses; use file-backed fields for large files.

### `HttpRequest`

Header: [../../include/FastNet/HttpServer.h](../../include/FastNet/HttpServer.h)

Fields:
- `method`
- `methodName`
- `target`
- `path`
- `queryString`
- `version`
- `queryParams`
- `headers`
- `body`
- `clientAddress`

Helpers:
- `getHeader(name)`

### `HttpServer`

Header: [../../include/FastNet/HttpServer.h](../../include/FastNet/HttpServer.h)

Lifecycle:
- `start(port, bindAddress, sslConfig)`
- `start(Address, sslConfig)`
- `stop()`

Routing:
- `registerHandler(path, method, handler)`
- `registerGet(path, handler)`
- `registerPost(path, handler)`
- `registerPut(path, handler)`
- `registerDelete(path, handler)`
- `registerPatch(path, handler)`
- `registerHead(path, handler)`
- `registerOptions(path, handler)`
- `registerStaticFileHandler(pathPrefix, directory)`
- `setRequestHandler(handler)`

Configuration:
- `setConnectionTimeout(timeoutMs)`
- `setRequestTimeout(timeoutMs)`
- `setWriteTimeout(timeoutMs)`
- `setMaxConnections(maxConnections)`
- `setMaxRequestSize(bytes)`
- `setStaticFileCacheLimit(bytes)`
- `setSSLConfig(sslConfig)`
- `setServerErrorCallback(callback)`

Status:
- `getClientCount()`
- `getClientIds()`
- `getListenAddress()`
- `isRunning()`

### `HttpClient`

Header: [../../include/FastNet/HttpClient.h](../../include/FastNet/HttpClient.h)

Connection:
- `connect(url, callback)`
- `disconnect()`

Requests:
- `get(path, headers, callback)`
- `head(path, headers, callback)`
- `post(path, headers, body, callback)`
- `put(path, headers, body, callback)`
- `patch(path, headers, body, callback)`
- `del(path, headers, callback)`
- `request(method, path, headers, body, callback)`

Configuration:
- `setConnectTimeout(timeoutMs)`
- `setRequestTimeout(timeoutMs)`
- `setReadTimeout(timeoutMs)`
- `setFollowRedirects(follow)`
- `setMaxRedirects(maxRedirects)`
- `setUseCompression(use)`
- `setSSLConfig(sslConfig)`

Status:
- `isConnected()`
- `getLocalAddress()`
- `getRemoteAddress()`
- `getLastError()`

## 7. WebSocket

### `WebSocketServer`

Header: [../../include/FastNet/WebSocketServer.h](../../include/FastNet/WebSocketServer.h)

Lifecycle:
- `start(port, bindAddress, sslConfig)`
- `stop()`

Sending:
- `sendTextToClient(clientId, message)`
- `sendBinaryToClient(clientId, data)`
- `broadcastText(message)`
- `broadcastBinary(data)`
- `disconnectClient(clientId, code, reason)`

Callbacks:
- `setClientConnectedCallback(callback)`
- `setClientDisconnectedCallback(callback)`
- `setMessageCallback(callback)`
- `setBinaryCallback(callback)`
- `setOwnedBinaryCallback(callback)`
- `setServerErrorCallback(callback)`

Configuration:
- `setConnectionTimeout(timeoutMs)`
- `setPingInterval(intervalMs)`
- `setMaxConnections(maxConnections)`

Status:
- `getClientCount()`
- `getClientIds()`
- `getClientAddress(clientId)`
- `getListenAddress()`
- `isRunning()`

### `WebSocketClient`

Header: [../../include/FastNet/WebSocketClient.h](../../include/FastNet/WebSocketClient.h)

Connection and Sending:
- `connect(url, callback)`
- `sendText(message)`
- `sendBinary(data)`
- `close(code, reason)`

Callbacks:
- `setConnectCallback(callback)`
- `setMessageCallback(callback)`
- `setBinaryCallback(callback)`
- `setOwnedBinaryCallback(callback)`
- `setErrorCallback(callback)`
- `setCloseCallback(callback)`

Configuration:
- `setConnectTimeout(timeoutMs)`
- `setPingInterval(intervalMs)`
- `setSSLConfig(sslConfig)`

Status:
- `isConnected()`
- `getLocalAddress()`
- `getRemoteAddress()`

## 8. Connection Pools and Service Managers

### `TcpConnectionPoolOptions`

Header: [../../include/FastNet/TcpConnectionPool.h](../../include/FastNet/TcpConnectionPool.h)

Fields:
- `minConnections`
- `maxConnections`
- `connectionTimeout`
- `acquireTimeout`
- `idleTimeout`
- `checkInterval`
- `sslConfig`

### `TcpConnectionPool`

Core Methods:
- `initialize()`
- `acquire(callback)`
- `acquireSync(connection)`
- `release(connection)`
- `shutdown()`
- `getCurrentConnectionCount()`
- `getIdleConnectionCount()`
- `getInUseConnectionCount()`

### `PooledConnection`

Common Methods:
- `getClient()`
- `getHost()`
- `getPort()`
- `isValid()`
- `getLastUsedTime()`
- `updateUsedTime()`
- `getState()`
- `setState(state)`
- `close()`

### `ConnectionManager`

Header: [../../include/FastNet/ConnectionManager.h](../../include/FastNet/ConnectionManager.h)

Lifecycle:
- `initialize()`
- `cleanup()`

Connection Allocation:
- `acquireConnection(service)`
- `acquireConnection(service, affinityKey)`
- `releaseConnection(id)`
- `closeConnection(id)`

Backend Management:
- `addBackendServer(service, host, port, weight)`
- `removeBackendServer(service, host, port)`
- `updateBackendWeight(service, host, port, weight)`

Load Balancing:
- `getLoadBalancingStrategy(service)`
- `setLoadBalancingStrategy(service, strategy)`

Status and Stats:
- `getPoolStats()`
- `getServiceStats(service)`
- `getServices()`
- `getBackendServers(service)`
- `getCircuitBreakerStats(service)`
- `reportBackendStatus(...)`
- `reportExecution(...)`

Strategy Enum:
- `RoundRobin`
- `Random`
- `LeastConnections`
- `WeightedRoundRobin`
- `IPHash`

## 9. Configuration, Logging, and Monitoring

### `Configuration`

Header: [../../include/FastNet/Configuration.h](../../include/FastNet/Configuration.h)

Load and Save:
- `loadFromFile(filename)`
- `saveToFile(filename)`
- `loadFromEnvironment()`
- `loadFromEnvironment(prefix)`

Read Options:
- `getString(option, defaultValue)`
- `getInt(option, defaultValue)`
- `getBool(option, defaultValue)`
- `getDouble(option, defaultValue)`

Write & Manage:
- `set(option, value)`
- `has(option)`
- `validate()`
- `validateOption(option, value)`
- `remove(option)`
- `clear()`
- `getAllKeys()`
- `snapshot()`
- `merge(other)`
- `merge(other, mode)`

Global Entrypoint:
- `getGlobalConfig()`

### `AsyncLogger`

Header: [../../include/FastNet/Logger.h](../../include/FastNet/Logger.h)

Core Methods:
- `AsyncLogger::getInstance()`
- `initialize(filePath, level, maxFileSize, mirrorToConsole)`
- `shutdown()`
- `flush()`
- `log(level, file, line, func, message)`
- `setLogLevel(level)`
- `getLogLevel()`
- `setConsoleMirror(enabled)`
- `isRunning()`

Helpers:
- `setGlobalLogLevel(level)`
- `getGlobalLogLevel()`
- `logLevelToString(level)`
- `logLevelFromString(text, fallback)`
- `getCurrentTimestamp()`
- `consoleLog(level, message)`

Logging Macros:
- `LOG_TRACE`, `LOG_DEBUG`, `LOG_INFO`, `LOG_WARN`, `LOG_ERROR`, `LOG_FATAL`
- `FASTNET_LOG_TRACE`, `FASTNET_LOG_DEBUG`, `FASTNET_LOG_INFO`, `FASTNET_LOG_WARN`, `FASTNET_LOG_ERROR`, `FASTNET_LOG_FATAL`

### `PerformanceMonitor`

Header: [../../include/FastNet/PerformanceMonitor.h](../../include/FastNet/PerformanceMonitor.h)

Core Methods:
- `initialize(enabled)`
- `shutdown()`
- `incrementMetric(name, value)`
- `setMetric(name, value)`
- `updateHistogram(name, value)`
- `recordTimer(name, milliseconds)`
- `startTimer()`
- `endTimer(name, timerId)`
- `getMetricValue(name)`
- `getMetricStats(name, min, max, avg)`
- `getMetricSnapshot(name, snapshot)`
- `snapshotMetrics()`
- `snapshot()`
- `resetMetric(name)`
- `resetAllMetrics()`
- `exportMetricsToJson()`
- `setEnabled(enabled)`
- `isEnabled()`
- `isInitialized()`

Global Entrypoint:
- `getPerformanceMonitor()`

Macros:
- `FASTNET_PERF_INCREMENT`
- `FASTNET_PERF_SET`
- `FASTNET_PERF_HISTOGRAM`
- `FASTNET_PERF_TIMER_START`
- `FASTNET_PERF_TIMER_END`
- `FASTNET_PERF_START_TIMER`
- `FASTNET_PERF_END_TIMER`

## 10. Error Model

Header: [../../include/FastNet/Error.h](../../include/FastNet/Error.h)

### `Error`

Constructors & Factories:
- `Error(code, message, systemCode, fileName, lineNumber, functionName)`
- `Error::success()`
- `Error::fromSystemError(code, message, fileName, lineNumber, functionName)`

Inspectors:
- `getCode()`
- `getMessage()`
- `getSystemCode()`
- `getFileName()`
- `getLineNumber()`
- `getFunctionName()`
- `isSuccess()`
- `isFailure()`
- `getSystemErrorMessage()`
- `toString()`
- `toStdErrorCode()`
- `Error::getErrorCodeName(code)`

Helpers & Macros:
- `errorCodeToString(code)`
- `systemErrorToNetworkError(systemError)`
- `boostErrorToNetworkError(ec)`
- `make_error_code(code)`
- `FASTNET_ERROR(code, message)`
- `FASTNET_SYSTEM_ERROR(code, message)`
- `FASTNET_SUCCESS`

### Exception Types
- `NetworkException`
- `SocketException`
- `ConnectionException`
- `ProtocolException`
- `SSLException`
- `TimeoutException`
- `AuthenticationException`
- `CompressionException`

### `ExceptionPolicy`

Strategies:
- `ThrowException`
- `ReturnErrorCode`
- `LogAndContinue`

Core Methods:
- `getInstance()`
- `setStrategy(strategy)`
- `getStrategy()`
- `enableExceptions()`
- `disableExceptions()`
- `shouldThrow()`
- `shouldLog()`
- `handle(error)`

### `Result<T>`

Common Methods:
- `Result<T>::success(value)`
- `Result<T>::error(error)`
- `Result<T>::error(code, message)`
- `isSuccess()`
- `isError()`
- `operator bool()`
- `value()`
- `error()`
- `errorCode()`
- `errorMessage()`
- `errorIfAny()`
- `valueOr(defaultValue)`

`Result<void>` is also available, useful for execution paths that only need to report success/failure.

## 11. Buffer and Memory Utilities

### `FastBuffer`

Header: [../../include/FastNet/FastBuffer.h](../../include/FastNet/FastBuffer.h)

Characteristics:
- Small buffer optimization (SBO) up to 4 KiB on the stack.
- Maximum capacity restricted to 64 MiB.
- Supports append, move, conversions to `std::vector<uint8_t>` or `std::string`.

Common Methods:
- `data()`
- `size()`
- `capacity()`
- `empty()`
- `usingHeapStorage()`
- `clear()`
- `reset()`
- `resize(size, fill)`
- `reserve(size)`
- `shrink_to_fit()`
- `push_back(byte)`
- `pop_back()`
- `append(...)`
- `assign(...)`
- `erase_front(len)`
- `toVector()`
- `toString()`
- `fromVector(buffer)`
- `fromString(text)`

### `MemoryPool<BlockSize>`

Header: [../../include/FastNet/MemoryPool.h](../../include/FastNet/MemoryPool.h)

Core Methods:
- `allocate()`
- `deallocate(ptr)`
- `warmUp(count)`
- `getAllocatedCount()`
- `getFreeCount()`
- `getTotalCount()`
- `getStats()`

### `BufferPool`

Core Methods:
- `BufferPool::getInstance()`
- `allocateBuffer(size)`
- `allocateReservedBuffer(reserveSize, initialSize)`
- `warmUp(smallCount, largeCount)`
- `getStats()`

## 12. Benchmark Utilities

Header: [../../include/FastNet/BenchmarkUtils.h](../../include/FastNet/BenchmarkUtils.h)

Common Methods:
- `getCurrentTime()`
- `now()`
- `formatBandwidth(bytes, seconds)`
- `formatLatency(milliseconds)`
- `formatBytes(bytes)`
- `formatOpsPerSecond(operations, seconds)`
- `formatDuration(milliseconds)`

## 13. Recommended Combinations

Private Protocol Services:
- `TcpServer`
- `TcpClient`
- `Timer`
- `AsyncLogger`

External APIs:
- `HttpServer`
- `HttpClient`
- `SSLConfig`
- `PerformanceMonitor`

Real-time Push:
- `HttpServer`
- `WebSocketServer`
- `Timer`

Multi-backend Clients:
- `ConnectionManager`
- `TcpConnectionPool`
- `TcpClient` or `HttpClient`
