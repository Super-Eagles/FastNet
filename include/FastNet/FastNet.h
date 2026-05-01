/**
 * @file FastNet.h
 * @brief FastNet public umbrella header
 */
#pragma once

#include "BenchmarkUtils.h"
#include "Config.h"
#include "Configuration.h"
#include "ConnectionManager.h"
#include "Error.h"
#include "base64.h"
#include "FastBuffer.h"
#include "HttpClient.h"
#include "HttpCommon.h"
#include "HttpParser.h"
#include "HttpServer.h"
#include "IoService.h"
#include "Logger.h"
#include "MemoryPool.h"
#include "PerformanceMonitor.h"
#include "SSLContext.h"
#include "TcpClient.h"
#include "TcpConnectionPool.h"
#include "TcpServer.h"
#include "Timer.h"
#include "UdpSocket.h"
#include "WebSocketClient.h"
#include "WebSocketProtocol.h"
#include "WebSocketServer.h"

namespace FastNet {

FASTNET_API ErrorCode initialize(size_t threadCount = 0);
FASTNET_API void cleanup();
FASTNET_API bool isInitialized();

} // namespace FastNet
