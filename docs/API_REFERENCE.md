# FastNet API 参考

这份文档按公开头文件整理常用 API。它用于快速查方法名和组合关系；完整签名以 `include/FastNet/*.h` 为准。

相关文档：

- [USER_GUIDE.md](USER_GUIDE.md)
- [COOKBOOK.md](COOKBOOK.md)
- [VALIDATION_CHECKLIST.md](VALIDATION_CHECKLIST.md)

## 1. 总入口

头文件：[../include/FastNet/FastNet.h](../include/FastNet/FastNet.h)

```cpp
FastNet::ErrorCode initialize(size_t threadCount = 0);
void cleanup();
bool isInitialized();
```

建议：

- 进程启动后尽早调用 `initialize()`。
- 大多数应用用 `getGlobalIoService()` 创建网络对象。
- 所有 server/client 停止后再调用 `cleanup()`。

## 2. 基础类型

头文件：[../include/FastNet/Config.h](../include/FastNet/Config.h)

### `Address`

字段：

- `std::string ip`
- `uint16_t port`

常用方法：

- `host()`
- `hasPort()`
- `normalizedHost()`
- `isValid()`
- `isIPv6()`
- `isLoopback()`
- `isAnyAddress()`
- `toString()`
- `Address::parse(endpoint, defaultPort)`

静态校验方法：

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

用于 TCP、UDP、WebSocket 二进制数据。

### `SSLConfig`

字段：

- `enableSSL`
- `certificateFile`
- `privateKeyFile`
- `caFile`
- `verifyPeer`
- `hostnameVerification`

TLS 需要以 `FASTNET_ENABLE_SSL=ON` 构建。

### `ErrorCode`

覆盖 socket、连接、绑定、监听、解析、超时、HTTP、WebSocket、SSL、认证、压缩等错误类别。

## 3. 运行时和定时器

### `IoService`

头文件：[../include/FastNet/IoService.h](../include/FastNet/IoService.h)

核心方法：

- `IoService(size_t threadCount = 0)`
- `start()`
- `stop()`
- `join()`
- `post(const Task&)`
- `post(Task&&)`
- `getPoller()`
- `isRunning()`
- `getThreadCount()`

全局入口：

- `configureGlobalIoService(threadCount)`
- `getGlobalIoService()`
- `shutdownGlobalIoService()`

### `TimerManager`

头文件：[../include/FastNet/Timer.h](../include/FastNet/Timer.h)

核心方法：

- `start()`
- `stop()`
- `isRunning()`
- `addTimer(delay, callback)`
- `addRepeatingTimer(interval, callback)`
- `cancelTimer(timerId)`
- `getActiveTimerCount()`

全局入口：

- `getGlobalTimerManager()`
- `shutdownGlobalTimerManager()`

### `Timer`

核心方法：

- `start(delay, callback, repeat = false)`
- `stop()`
- `isRunning()`

### `ConnectionTimeoutManager`

核心方法：

- `setConnectionTimeout(connId, timeout, callback)`
- `refreshConnection(connId)`
- `removeConnection(connId)`
- `getManagedConnectionCount()`

## 4. TCP

### `TcpServer`

头文件：[../include/FastNet/TcpServer.h](../include/FastNet/TcpServer.h)

生命周期：

- `TcpServer(IoService&)`
- `start(port, bindAddress, sslConfig)`
- `start(Address, sslConfig)`
- `stop()`

发送：

- `sendToClient(clientId, const Buffer&)`
- `sendToClient(clientId, Buffer&&)`
- `sendToClient(clientId, shared_ptr<const Buffer>)`
- `sendToClient(clientId, string&&)`
- `sendToClient(clientId, string_view)`
- `sendFileToClient(clientId, prefix, filePath, offset, length)`
- `broadcast(...)`

连接控制：

- `disconnectClient(clientId)`
- `closeClientAfterPendingWrites(clientId)`

状态：

- `getClientCount()`
- `getClientIds()`
- `getClientAddress(clientId)`
- `hasClient(clientId)`
- `getListenAddress()`
- `isRunning()`

回调：

- `setClientConnectedCallback(callback)`
- `setClientDisconnectedCallback(callback)`
- `setDataReceivedCallback(callback)`
- `setOwnedDataReceivedCallback(callback)`
- `setSharedDataReceivedCallback(callback)`
- `setServerErrorCallback(callback)`

配置：

- `setConnectionTimeout(timeoutMs)`
- `setReadTimeout(timeoutMs)`
- `setWriteTimeout(timeoutMs)`
- `setMaxConnections(maxConnections)`

### `TcpClient`

头文件：[../include/FastNet/TcpClient.h](../include/FastNet/TcpClient.h)

连接：

- `connect(host, port, callback, sslConfig)`
- `connect(Address, callback, sslConfig)`
- `disconnect()`
- `disconnectAfterPendingWrites()`

发送：

- `send(const Buffer&)`
- `send(Buffer&&)`
- `send(shared_ptr<const Buffer>)`
- `send(string&&)`
- `send(string_view)`
- `send(shared_ptr<const string>)`

回调：

- `setConnectCallback(callback)`
- `setDisconnectCallback(callback)`
- `setDataReceivedCallback(callback)`
- `setOwnedDataReceivedCallback(callback)`
- `setSharedDataReceivedCallback(callback)`
- `setErrorCallback(callback)`

状态：

- `getLocalAddress()`
- `getRemoteAddress()`
- `isConnected()`
- `isSecure()`
- `getPendingWriteBytes()`
- `getLastError()`

超时：

- `setConnectTimeout(timeoutMs)`
- `setReadTimeout(timeoutMs)`
- `setWriteTimeout(timeoutMs)`

## 5. UDP

### `UdpSocket`

头文件：[../include/FastNet/UdpSocket.h](../include/FastNet/UdpSocket.h)

生命周期：

- `bind(port, bindAddress)`
- `bind(Address)`
- `startReceive()`
- `stopReceive()`

发送：

- `sendTo(Address, const Buffer&)`
- `sendTo(Address, string_view)`

回调和配置：

- `setDataReceivedCallback(callback)`
- `setErrorCallback(callback)`
- `setReceiveBufferSize(size)`
- `setSendBufferSize(size)`
- `setBroadcast(enable)`

状态：

- `getLocalAddress()`
- `isBound()`
- `isReceiving()`

## 6. HTTP

### `HttpResponse`

头文件：[../include/FastNet/HttpCommon.h](../include/FastNet/HttpCommon.h)

字段：

- `statusCode`
- `statusMessage`
- `headers`
- `body`
- `hasFileBody`
- `filePath`
- `fileOffset`
- `fileLength`

小响应直接填 `body`；大文件优先用 file-backed 字段。

### `HttpRequest`

头文件：[../include/FastNet/HttpServer.h](../include/FastNet/HttpServer.h)

字段：

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

辅助方法：

- `getHeader(name)`

### `HttpServer`

头文件：[../include/FastNet/HttpServer.h](../include/FastNet/HttpServer.h)

生命周期：

- `start(port, bindAddress, sslConfig)`
- `start(Address, sslConfig)`
- `stop()`

路由：

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

配置：

- `setConnectionTimeout(timeoutMs)`
- `setRequestTimeout(timeoutMs)`
- `setWriteTimeout(timeoutMs)`
- `setMaxConnections(maxConnections)`
- `setMaxRequestSize(bytes)`
- `setStaticFileCacheLimit(bytes)`
- `setSSLConfig(sslConfig)`
- `setServerErrorCallback(callback)`

状态：

- `getClientCount()`
- `getClientIds()`
- `getListenAddress()`
- `isRunning()`

### `HttpClient`

头文件：[../include/FastNet/HttpClient.h](../include/FastNet/HttpClient.h)

连接：

- `connect(url, callback)`
- `disconnect()`

请求：

- `get(path, headers, callback)`
- `head(path, headers, callback)`
- `post(path, headers, body, callback)`
- `put(path, headers, body, callback)`
- `patch(path, headers, body, callback)`
- `del(path, headers, callback)`
- `request(method, path, headers, body, callback)`

配置：

- `setConnectTimeout(timeoutMs)`
- `setRequestTimeout(timeoutMs)`
- `setReadTimeout(timeoutMs)`
- `setFollowRedirects(follow)`
- `setMaxRedirects(maxRedirects)`
- `setUseCompression(use)`
- `setSSLConfig(sslConfig)`

状态：

- `isConnected()`
- `getLocalAddress()`
- `getRemoteAddress()`
- `getLastError()`

## 7. WebSocket

### `WebSocketServer`

头文件：[../include/FastNet/WebSocketServer.h](../include/FastNet/WebSocketServer.h)

生命周期：

- `start(port, bindAddress, sslConfig)`
- `stop()`

发送：

- `sendTextToClient(clientId, message)`
- `sendBinaryToClient(clientId, data)`
- `broadcastText(message)`
- `broadcastBinary(data)`
- `disconnectClient(clientId, code, reason)`

回调：

- `setClientConnectedCallback(callback)`
- `setClientDisconnectedCallback(callback)`
- `setMessageCallback(callback)`
- `setBinaryCallback(callback)`
- `setOwnedBinaryCallback(callback)`
- `setServerErrorCallback(callback)`

配置：

- `setConnectionTimeout(timeoutMs)`
- `setPingInterval(intervalMs)`
- `setMaxConnections(maxConnections)`

状态：

- `getClientCount()`
- `getClientIds()`
- `getClientAddress(clientId)`
- `getListenAddress()`
- `isRunning()`

### `WebSocketClient`

头文件：[../include/FastNet/WebSocketClient.h](../include/FastNet/WebSocketClient.h)

连接与发送：

- `connect(url, callback)`
- `sendText(message)`
- `sendBinary(data)`
- `close(code, reason)`

回调：

- `setConnectCallback(callback)`
- `setMessageCallback(callback)`
- `setBinaryCallback(callback)`
- `setOwnedBinaryCallback(callback)`
- `setErrorCallback(callback)`
- `setCloseCallback(callback)`

配置：

- `setConnectTimeout(timeoutMs)`
- `setPingInterval(intervalMs)`
- `setSSLConfig(sslConfig)`

状态：

- `isConnected()`
- `getLocalAddress()`
- `getRemoteAddress()`

## 8. 连接池和服务调度

### `TcpConnectionPoolOptions`

头文件：[../include/FastNet/TcpConnectionPool.h](../include/FastNet/TcpConnectionPool.h)

字段：

- `minConnections`
- `maxConnections`
- `connectionTimeout`
- `acquireTimeout`
- `idleTimeout`
- `checkInterval`
- `sslConfig`

### `TcpConnectionPool`

核心方法：

- `initialize()`
- `acquire(callback)`
- `acquireSync(connection)`
- `release(connection)`
- `shutdown()`
- `getCurrentConnectionCount()`
- `getIdleConnectionCount()`
- `getInUseConnectionCount()`

### `PooledConnection`

常用方法：

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

头文件：[../include/FastNet/ConnectionManager.h](../include/FastNet/ConnectionManager.h)

生命周期：

- `initialize()`
- `cleanup()`

连接分配：

- `acquireConnection(service)`
- `acquireConnection(service, affinityKey)`
- `releaseConnection(id)`
- `closeConnection(id)`

后端管理：

- `addBackendServer(service, host, port, weight)`
- `removeBackendServer(service, host, port)`
- `updateBackendWeight(service, host, port, weight)`

负载均衡：

- `getLoadBalancingStrategy(service)`
- `setLoadBalancingStrategy(service, strategy)`

状态与统计：

- `getPoolStats()`
- `getServiceStats(service)`
- `getServices()`
- `getBackendServers(service)`
- `getCircuitBreakerStats(service)`
- `reportBackendStatus(...)`
- `reportExecution(...)`

策略枚举：

- `RoundRobin`
- `Random`
- `LeastConnections`
- `WeightedRoundRobin`
- `IPHash`

## 9. 配置、日志和监控

### `Configuration`

头文件：[../include/FastNet/Configuration.h](../include/FastNet/Configuration.h)

加载与保存：

- `loadFromFile(filename)`
- `saveToFile(filename)`
- `loadFromEnvironment()`
- `loadFromEnvironment(prefix)`

读取：

- `getString(option, defaultValue)`
- `getInt(option, defaultValue)`
- `getBool(option, defaultValue)`
- `getDouble(option, defaultValue)`

写入与管理：

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

全局入口：

- `getGlobalConfig()`

### `AsyncLogger`

头文件：[../include/FastNet/Logger.h](../include/FastNet/Logger.h)

核心方法：

- `AsyncLogger::getInstance()`
- `initialize(filePath, level, maxFileSize, mirrorToConsole)`
- `shutdown()`
- `flush()`
- `log(level, file, line, func, message)`
- `setLogLevel(level)`
- `getLogLevel()`
- `setConsoleMirror(enabled)`
- `isRunning()`

辅助函数：

- `setGlobalLogLevel(level)`
- `getGlobalLogLevel()`
- `logLevelToString(level)`
- `logLevelFromString(text, fallback)`
- `getCurrentTimestamp()`
- `consoleLog(level, message)`

日志宏：

- `LOG_TRACE`
- `LOG_DEBUG`
- `LOG_INFO`
- `LOG_WARN`
- `LOG_ERROR`
- `LOG_FATAL`
- `FASTNET_LOG_TRACE`
- `FASTNET_LOG_DEBUG`
- `FASTNET_LOG_INFO`
- `FASTNET_LOG_WARN`
- `FASTNET_LOG_ERROR`
- `FASTNET_LOG_FATAL`

### `PerformanceMonitor`

头文件：[../include/FastNet/PerformanceMonitor.h](../include/FastNet/PerformanceMonitor.h)

核心方法：

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

全局入口：

- `getPerformanceMonitor()`

宏：

- `FASTNET_PERF_INCREMENT`
- `FASTNET_PERF_SET`
- `FASTNET_PERF_HISTOGRAM`
- `FASTNET_PERF_TIMER_START`
- `FASTNET_PERF_TIMER_END`
- `FASTNET_PERF_START_TIMER`
- `FASTNET_PERF_END_TIMER`

## 10. 错误模型

头文件：[../include/FastNet/Error.h](../include/FastNet/Error.h)

### `Error`

构造和工厂：

- `Error(code, message, systemCode, fileName, lineNumber, functionName)`
- `Error::success()`
- `Error::fromSystemError(code, message, fileName, lineNumber, functionName)`

查询：

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

辅助函数和宏：

- `errorCodeToString(code)`
- `systemErrorToNetworkError(systemError)`
- `boostErrorToNetworkError(ec)`
- `make_error_code(code)`
- `FASTNET_ERROR(code, message)`
- `FASTNET_SYSTEM_ERROR(code, message)`
- `FASTNET_SUCCESS`

### 异常类型

- `NetworkException`
- `SocketException`
- `ConnectionException`
- `ProtocolException`
- `SSLException`
- `TimeoutException`
- `AuthenticationException`
- `CompressionException`

### `ExceptionPolicy`

策略：

- `ThrowException`
- `ReturnErrorCode`
- `LogAndContinue`

核心方法：

- `getInstance()`
- `setStrategy(strategy)`
- `getStrategy()`
- `enableExceptions()`
- `disableExceptions()`
- `shouldThrow()`
- `shouldLog()`
- `handle(error)`

### `Result<T>`

常用方法：

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

`Result<void>` 同样可用，适合只需要表达成功/失败的路径。

## 11. 缓冲和内存工具

### `FastBuffer`

头文件：[../include/FastNet/FastBuffer.h](../include/FastNet/FastBuffer.h)

特点：

- 4 KiB 栈内小缓冲优化。
- 最大容量限制为 64 MiB。
- 支持追加、移动、转换为 `std::vector<uint8_t>` 或 `std::string`。

常用方法：

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

头文件：[../include/FastNet/MemoryPool.h](../include/FastNet/MemoryPool.h)

核心方法：

- `allocate()`
- `deallocate(ptr)`
- `warmUp(count)`
- `getAllocatedCount()`
- `getFreeCount()`
- `getTotalCount()`
- `getStats()`

### `BufferPool`

核心方法：

- `BufferPool::getInstance()`
- `allocateBuffer(size)`
- `allocateReservedBuffer(reserveSize, initialSize)`
- `warmUp(smallCount, largeCount)`
- `getStats()`

## 12. Benchmark 工具

头文件：[../include/FastNet/BenchmarkUtils.h](../include/FastNet/BenchmarkUtils.h)

常用方法：

- `getCurrentTime()`
- `now()`
- `formatBandwidth(bytes, seconds)`
- `formatLatency(milliseconds)`
- `formatBytes(bytes)`
- `formatOpsPerSecond(operations, seconds)`
- `formatDuration(milliseconds)`

## 13. 推荐组合

私有协议服务：

- `TcpServer`
- `TcpClient`
- `Timer`
- `AsyncLogger`

对外 API：

- `HttpServer`
- `HttpClient`
- `SSLConfig`
- `PerformanceMonitor`

实时推送：

- `HttpServer`
- `WebSocketServer`
- `Timer`

多后端客户端：

- `ConnectionManager`
- `TcpConnectionPool`
- `TcpClient` 或 `HttpClient`
