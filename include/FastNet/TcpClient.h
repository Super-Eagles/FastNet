/**
 * @file TcpClient.h
 * @brief FastNet TCP client API
 */
#pragma once

#include "Config.h"
#include "Error.h"
#include "IoService.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace FastNet {

using ConnectCallback = std::function<void(bool success, const std::string& errorMessage)>;
using DisconnectCallback = std::function<void(const std::string& reason)>;
using DataReceivedCallback = std::function<void(const Buffer& data)>;
using OwnedDataReceivedCallback = std::function<void(Buffer&& data)>;
using SharedDataReceivedCallback = std::function<void(const std::shared_ptr<const Buffer>& data)>;
using ErrorCallback = std::function<void(ErrorCode code, const std::string& errorMessage)>;

class FASTNET_API TcpClient {
public:
    explicit TcpClient(IoService& ioService);
    ~TcpClient();

    bool connect(const std::string& host,
                 uint16_t port,
                 const ConnectCallback& callback,
                 const SSLConfig& sslConfig = SSLConfig());
    bool connect(const Address& remoteAddress,
                 const ConnectCallback& callback,
                 const SSLConfig& sslConfig = SSLConfig());
    void disconnect();
    void disconnectAfterPendingWrites();
    bool send(const Buffer& data);
    bool send(Buffer&& data);
    bool send(const std::shared_ptr<const Buffer>& data);
    bool send(std::string&& data);
    bool send(std::string_view data);
    bool send(const std::shared_ptr<const std::string>& data);

    void setConnectCallback(const ConnectCallback& callback);
    void setDisconnectCallback(const DisconnectCallback& callback);
    void setDataReceivedCallback(const DataReceivedCallback& callback);
    void setOwnedDataReceivedCallback(const OwnedDataReceivedCallback& callback);
    void setSharedDataReceivedCallback(const SharedDataReceivedCallback& callback);
    void setErrorCallback(const ErrorCallback& callback);

    Address getLocalAddress() const;
    Address getRemoteAddress() const;
    bool isConnected() const;
    bool isSecure() const;
    size_t getPendingWriteBytes() const;
    Error getLastError() const;

    void setConnectTimeout(uint32_t timeoutMs);
    void setReadTimeout(uint32_t timeoutMs);
    void setWriteTimeout(uint32_t timeoutMs);

private:
    class Impl;
    std::shared_ptr<Impl> impl_;

    ConnectCallback connectCallback_;
    DisconnectCallback disconnectCallback_;
    DataReceivedCallback dataReceivedCallback_;
    OwnedDataReceivedCallback ownedDataReceivedCallback_;
    SharedDataReceivedCallback sharedDataReceivedCallback_;
    ErrorCallback errorCallback_;
};

} // namespace FastNet
