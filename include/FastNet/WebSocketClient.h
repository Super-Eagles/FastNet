/**
 * @file WebSocketClient.h
 * @brief FastNet WebSocket client API
 */
#pragma once

#include "Config.h"
#include "IoService.h"

#include <functional>
#include <memory>
#include <string>

namespace FastNet {

using WebSocketConnectCallback = std::function<void(bool success, const std::string& errorMessage)>;
using WebSocketMessageCallback = std::function<void(const std::string& message)>;
using WebSocketBinaryCallback = std::function<void(const Buffer& data)>;
using WebSocketBinaryOwnedCallback = std::function<void(Buffer&& data)>;
using WebSocketErrorCallback = std::function<void(ErrorCode code, const std::string& errorMessage)>;
using WebSocketCloseCallback = std::function<void(uint16_t code, const std::string& reason)>;

class FASTNET_API WebSocketClient {
public:
    explicit WebSocketClient(IoService& ioService);
    ~WebSocketClient();

    bool connect(const std::string& url, const WebSocketConnectCallback& callback);
    bool sendText(const std::string& message);
    bool sendBinary(const Buffer& data);
    void close(uint16_t code = 1000, const std::string& reason = "Normal closure");

    void setConnectCallback(const WebSocketConnectCallback& callback);
    void setMessageCallback(const WebSocketMessageCallback& callback);
    void setBinaryCallback(const WebSocketBinaryCallback& callback);
    void setOwnedBinaryCallback(const WebSocketBinaryOwnedCallback& callback);
    void setErrorCallback(const WebSocketErrorCallback& callback);
    void setCloseCallback(const WebSocketCloseCallback& callback);
    void setConnectTimeout(uint32_t timeoutMs);
    void setPingInterval(uint32_t intervalMs);
    void setSSLConfig(const SSLConfig& sslConfig);

    bool isConnected() const;
    Address getLocalAddress() const;
    Address getRemoteAddress() const;

private:
    class Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace FastNet
