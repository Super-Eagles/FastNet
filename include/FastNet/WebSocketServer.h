/**
 * @file WebSocketServer.h
 * @brief FastNet WebSocket server API
 */
#pragma once

#include "Config.h"
#include "Error.h"
#include "IoService.h"

#include <functional>
#include <memory>
#include <vector>

namespace FastNet {

using WebSocketServerClientConnectedCallback = std::function<void(ConnectionId clientId, const Address& clientAddress)>;
using WebSocketServerClientDisconnectedCallback =
    std::function<void(ConnectionId clientId, uint16_t code, const std::string& reason)>;
using WebSocketServerMessageCallback = std::function<void(ConnectionId clientId, const std::string& message)>;
using WebSocketServerBinaryCallback = std::function<void(ConnectionId clientId, const Buffer& data)>;
using WebSocketServerBinaryOwnedCallback = std::function<void(ConnectionId clientId, Buffer&& data)>;
using WebSocketServerErrorCallback = std::function<void(const Error& error)>;

class FASTNET_API WebSocketServer {
public:
    explicit WebSocketServer(IoService& ioService);
    ~WebSocketServer();

    Error start(uint16_t port,
                const std::string& bindAddress = "0.0.0.0",
                const SSLConfig& sslConfig = SSLConfig());
    void stop();

    Error sendTextToClient(ConnectionId clientId, const std::string& message);
    Error sendBinaryToClient(ConnectionId clientId, const Buffer& data);
    void broadcastText(const std::string& message);
    void broadcastBinary(const Buffer& data);
    void disconnectClient(ConnectionId clientId,
                          uint16_t code = 1000,
                          const std::string& reason = "Normal closure");

    size_t getClientCount() const;
    std::vector<ConnectionId> getClientIds() const;
    Address getClientAddress(ConnectionId clientId) const;

    void setClientConnectedCallback(const WebSocketServerClientConnectedCallback& callback);
    void setClientDisconnectedCallback(const WebSocketServerClientDisconnectedCallback& callback);
    void setMessageCallback(const WebSocketServerMessageCallback& callback);
    void setBinaryCallback(const WebSocketServerBinaryCallback& callback);
    void setOwnedBinaryCallback(const WebSocketServerBinaryOwnedCallback& callback);
    void setServerErrorCallback(const WebSocketServerErrorCallback& callback);
    void setConnectionTimeout(uint32_t timeoutMs);
    void setPingInterval(uint32_t intervalMs);
    void setMaxConnections(size_t maxConnections);

    Address getListenAddress() const;
    bool isRunning() const;

private:
    class Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace FastNet
