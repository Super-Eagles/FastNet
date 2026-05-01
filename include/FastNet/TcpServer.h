/**
 * @file TcpServer.h
 * @brief FastNet TCP server API
 */
#pragma once

#include "Config.h"
#include "Error.h"
#include "IoService.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>
#include <vector>

namespace FastNet {

using ServerClientConnectedCallback = std::function<void(ConnectionId clientId, const Address& clientAddress)>;
using ServerClientDisconnectedCallback = std::function<void(ConnectionId clientId, const std::string& reason)>;
using ServerDataReceivedCallback = std::function<void(ConnectionId clientId, const Buffer& data)>;
using ServerDataReceivedOwnedCallback = std::function<void(ConnectionId clientId, Buffer&& data)>;
using ServerDataReceivedSharedCallback =
    std::function<void(ConnectionId clientId, const std::shared_ptr<const Buffer>& data)>;
using ServerErrorCallback = std::function<void(const Error& error)>;

class FASTNET_API TcpServer {
public:
    explicit TcpServer(IoService& ioService);
    ~TcpServer();

    Error start(uint16_t port,
                const std::string& bindAddress = "0.0.0.0",
                const SSLConfig& sslConfig = SSLConfig());
    Error start(const Address& listenAddress, const SSLConfig& sslConfig = SSLConfig());
    void stop();

    Error sendToClient(ConnectionId clientId, const Buffer& data);
    Error sendToClient(ConnectionId clientId, Buffer&& data);
    Error sendToClient(ConnectionId clientId, const std::shared_ptr<const Buffer>& data);
    Error sendToClient(ConnectionId clientId, std::string&& data);
    Error sendToClient(ConnectionId clientId, std::string_view data);
    Error sendFileToClient(ConnectionId clientId,
                           const Buffer& prefix,
                           const std::string& filePath,
                           uint64_t offset = 0,
                           uint64_t length = 0);
    Error sendFileToClient(ConnectionId clientId,
                           std::string&& prefix,
                           const std::string& filePath,
                           uint64_t offset = 0,
                           uint64_t length = 0);
    Error sendFileToClient(ConnectionId clientId,
                            std::string_view prefix,
                            const std::string& filePath,
                            uint64_t offset = 0,
                            uint64_t length = 0);
    void broadcast(const Buffer& data);
    void broadcast(Buffer&& data);
    void broadcast(std::string&& data);
    void broadcast(std::string_view data);
    void disconnectClient(ConnectionId clientId);
    void closeClientAfterPendingWrites(ConnectionId clientId);

    size_t getClientCount() const;
    std::vector<ConnectionId> getClientIds() const;
    Address getClientAddress(ConnectionId clientId) const;
    bool hasClient(ConnectionId clientId) const;

    void setClientConnectedCallback(const ServerClientConnectedCallback& callback);
    void setClientDisconnectedCallback(const ServerClientDisconnectedCallback& callback);
    void setDataReceivedCallback(const ServerDataReceivedCallback& callback);
    void setOwnedDataReceivedCallback(const ServerDataReceivedOwnedCallback& callback);
    void setSharedDataReceivedCallback(const ServerDataReceivedSharedCallback& callback);
    void setServerErrorCallback(const ServerErrorCallback& callback);
    void setConnectionTimeout(uint32_t timeoutMs);
    void setReadTimeout(uint32_t timeoutMs);
    void setWriteTimeout(uint32_t timeoutMs);
    void setMaxConnections(size_t maxConnections);

    Address getListenAddress() const;
    bool isRunning() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;

    ServerClientConnectedCallback clientConnectedCallback_;
    ServerClientDisconnectedCallback clientDisconnectedCallback_;
    ServerDataReceivedCallback dataReceivedCallback_;
    ServerDataReceivedOwnedCallback ownedDataReceivedCallback_;
    ServerDataReceivedSharedCallback sharedDataReceivedSharedCallback_;
    ServerErrorCallback serverErrorCallback_;
};

} // namespace FastNet
