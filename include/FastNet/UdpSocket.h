/**
 * @file UdpSocket.h
 * @brief FastNet UDP socket API
 */
#pragma once

#include "Config.h"
#include "IoService.h"

#include <functional>
#include <memory>
#include <string_view>

namespace FastNet {

using UdpDataReceivedCallback = std::function<void(const Address& sender, const Buffer& data)>;
using UdpErrorCallback = std::function<void(ErrorCode code, const std::string& errorMessage)>;

class FASTNET_API UdpSocket {
public:
    explicit UdpSocket(IoService& ioService);
    ~UdpSocket();

    bool bind(uint16_t port, const std::string& bindAddress = "0.0.0.0");
    bool bind(const Address& localAddress);
    bool sendTo(const Address& destination, const Buffer& data);
    bool sendTo(const Address& destination, std::string_view data);

    bool startReceive();
    void stopReceive();

    Address getLocalAddress() const;
    bool isBound() const;
    bool isReceiving() const;

    void setDataReceivedCallback(const UdpDataReceivedCallback& callback);
    void setErrorCallback(const UdpErrorCallback& callback);
    void setReceiveBufferSize(size_t size);
    void setSendBufferSize(size_t size);
    void setBroadcast(bool enable);

private:
    class Impl;
    std::shared_ptr<Impl> impl_;

    UdpDataReceivedCallback dataReceivedCallback_;
    UdpErrorCallback errorCallback_;
};

} // namespace FastNet
