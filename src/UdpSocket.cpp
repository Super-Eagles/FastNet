/**
 * @file UdpSocket.cpp
 * @brief FastNet UDP socket implementation
 */
#include "UdpSocket.h"

#include "SocketWrapper.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

namespace FastNet {
namespace {

bool isWouldBlockError(int errorCode) noexcept {
#ifdef _WIN32
    return errorCode == WSAEWOULDBLOCK;
#else
    return errorCode == EAGAIN || errorCode == EWOULDBLOCK;
#endif
}

} // namespace

class UdpSocket::Impl : public std::enable_shared_from_this<UdpSocket::Impl> {
public:
    explicit Impl(IoService& ioService)
        : ioService_(ioService),
          receiveScratch_(64 * 1024) {}

    ~Impl() {
        stopReceive();
        socket_.close();
    }

    bool bind(const Address& localAddress) {
        if (bound_.load(std::memory_order_acquire)) {
            return false;
        }

        Error result = socket_.create(SocketType::UDP, localAddress, true);
        if (result.isFailure()) {
            notifyError(result.getCode(), result.getMessage());
            return false;
        }

        SocketOption option;
        option.reuseAddr = true;
        option.broadcast = broadcastEnabled_.load(std::memory_order_acquire);
        result = socket_.setOption(option);
        if (result.isFailure()) {
            notifyError(result.getCode(), result.getMessage());
            socket_.close();
            return false;
        }

        result = socket_.bind(localAddress);
        if (result.isFailure()) {
            notifyError(result.getCode(), result.getMessage());
            socket_.close();
            return false;
        }

        result = socket_.setNonBlocking(true);
        if (result.isFailure()) {
            notifyError(result.getCode(), result.getMessage());
            socket_.close();
            return false;
        }

        result = socket_.setRecvBufferSize(static_cast<int>(recvBufferSize_.load(std::memory_order_acquire)));
        if (result.isFailure()) {
            notifyError(result.getCode(), result.getMessage());
            socket_.close();
            return false;
        }

        result = socket_.setSendBufferSize(static_cast<int>(sendBufferSize_.load(std::memory_order_acquire)));
        if (result.isFailure()) {
            notifyError(result.getCode(), result.getMessage());
            socket_.close();
            return false;
        }

        std::string ip;
        uint16_t port = localAddress.port;
        if (socket_.getLocalAddress(ip, port).isSuccess()) {
            localAddress_ = Address(ip, port);
        } else {
            localAddress_ = localAddress;
        }

        bound_.store(true, std::memory_order_release);
        return true;
    }

    bool sendTo(const Address& destination, const Buffer& data) {
        if (!bound_.load(std::memory_order_acquire)) {
            notifyError(ErrorCode::InvalidArgument, "UDP socket is not bound");
            return false;
        }

        const int sent = socket_.sendTo(data.data(), data.size(), destination);
        if (sent < 0) {
            const int errorCode = SocketWrapper::getLastSocketError();
            if (!isWouldBlockError(errorCode)) {
                notifyError(ErrorCode::SocketError,
                            "UDP send failed: " + SocketWrapper::getErrorMessage(errorCode));
            }
            return false;
        }

        return static_cast<size_t>(sent) == data.size();
    }

    bool startReceive() {
        if (!bound_.load(std::memory_order_acquire)) {
            return false;
        }
        bool expected = false;
        if (!receiving_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            return false;
        }

        std::weak_ptr<Impl> weakSelf = shared_from_this();
        if (!ioService_.getPoller().add(
                socket_.getFd(),
                EventType::Read | EventType::Error | EventType::Close,
                [weakSelf](socket_t, EventType events, void*, size_t) {
                    if (auto self = weakSelf.lock()) {
                        self->handlePollEvents(events);
                    }
                })) {
            receiving_.store(false, std::memory_order_release);
            notifyError(ErrorCode::SocketError, "Failed to register UDP socket with the event poller");
            return false;
        }
        return true;
    }

    void stopReceive() {
        if (!receiving_.exchange(false, std::memory_order_acq_rel)) {
            return;
        }
        if (bound_.load(std::memory_order_acquire)) {
            ioService_.getPoller().remove(socket_.getFd());
        }
    }

    Address getLocalAddress() const {
        return localAddress_;
    }

    bool isBound() const {
        return bound_.load(std::memory_order_acquire);
    }

    bool isReceiving() const {
        return receiving_.load(std::memory_order_acquire);
    }

    void setCallbacks(const UdpDataReceivedCallback& dataCallback, const UdpErrorCallback& errorCallback) {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        dataReceivedCallback_ = dataCallback;
        errorCallback_ = errorCallback;
    }

    void setReceiveBufferSize(size_t size) {
        const size_t normalizedSize = std::max<size_t>(size, 1);
        recvBufferSize_.store(normalizedSize, std::memory_order_release);
        if (bound_.load(std::memory_order_acquire)) {
            const Error result = socket_.setRecvBufferSize(static_cast<int>(normalizedSize));
            if (result.isFailure()) {
                notifyError(result.getCode(), result.getMessage());
            }
        }
    }

    void setSendBufferSize(size_t size) {
        const size_t normalizedSize = std::max<size_t>(size, 1);
        sendBufferSize_.store(normalizedSize, std::memory_order_release);
        if (bound_.load(std::memory_order_acquire)) {
            const Error result = socket_.setSendBufferSize(static_cast<int>(normalizedSize));
            if (result.isFailure()) {
                notifyError(result.getCode(), result.getMessage());
            }
        }
    }

    void setBroadcast(bool enable) {
        broadcastEnabled_.store(enable, std::memory_order_release);
        if (bound_.load(std::memory_order_acquire)) {
            const Error result = socket_.setBroadcast(enable);
            if (result.isFailure()) {
                notifyError(result.getCode(), result.getMessage());
            }
        }
    }

private:
    void handlePollEvents(EventType events) {
        if (!receiving_.load(std::memory_order_acquire)) {
            return;
        }

        if (hasEvent(events, EventType::Read)) {
            drainReceiveQueue();
        }
        if (hasEvent(events, EventType::Error) || hasEvent(events, EventType::Close)) {
            notifyError(ErrorCode::SocketError, "UDP socket reported an error");
        }
    }

    void drainReceiveQueue() {
        const size_t bufferSize = recvBufferSize_.load(std::memory_order_acquire);
        if (receiveScratch_.size() < bufferSize) {
            receiveScratch_.resize(bufferSize);
        }
        while (receiving_.load(std::memory_order_acquire)) {
            std::string fromIp;
            uint16_t fromPort = 0;
            const int received = socket_.recvFrom(receiveScratch_.data(), bufferSize, fromIp, fromPort);
            if (received >= 0) {
                UdpDataReceivedCallback callbackCopy;
                {
                    std::lock_guard<std::mutex> lock(callbackMutex_);
                    callbackCopy = dataReceivedCallback_;
                }

                if (callbackCopy) {
                    Buffer packet(receiveScratch_.begin(), receiveScratch_.begin() + received);
                    callbackCopy(Address(fromIp, fromPort), packet);
                }
                continue;
            }

            const int errorCode = SocketWrapper::getLastSocketError();
            if (isWouldBlockError(errorCode)) {
                break;
            }

            notifyError(ErrorCode::SocketError,
                        "UDP receive failed: " + SocketWrapper::getErrorMessage(errorCode));
            break;
        }
    }

    void notifyError(ErrorCode code, const std::string& message) {
        UdpErrorCallback callbackCopy;
        {
            std::lock_guard<std::mutex> lock(callbackMutex_);
            callbackCopy = errorCallback_;
        }
        if (callbackCopy) {
            callbackCopy(code, message);
        }
    }

    IoService& ioService_;
    SocketWrapper socket_;
    std::atomic<bool> bound_{false};
    std::atomic<bool> receiving_{false};
    Address localAddress_;
    std::atomic<size_t> recvBufferSize_{64 * 1024};
    std::atomic<size_t> sendBufferSize_{64 * 1024};
    std::vector<uint8_t> receiveScratch_;
    std::atomic<bool> broadcastEnabled_{false};
    mutable std::mutex callbackMutex_;
    UdpDataReceivedCallback dataReceivedCallback_;
    UdpErrorCallback errorCallback_;
};

UdpSocket::UdpSocket(IoService& ioService)
    : impl_(std::make_shared<Impl>(ioService)) {}

UdpSocket::~UdpSocket() = default;

bool UdpSocket::bind(uint16_t port, const std::string& bindAddress) {
    return bind(Address(bindAddress, port));
}

bool UdpSocket::bind(const Address& localAddress) {
    return impl_->bind(localAddress);
}

bool UdpSocket::sendTo(const Address& destination, const Buffer& data) {
    return impl_->sendTo(destination, data);
}

bool UdpSocket::sendTo(const Address& destination, std::string_view data) {
    return impl_->sendTo(destination, Buffer(data.begin(), data.end()));
}

bool UdpSocket::startReceive() {
    return impl_->startReceive();
}

void UdpSocket::stopReceive() {
    impl_->stopReceive();
}

Address UdpSocket::getLocalAddress() const {
    return impl_->getLocalAddress();
}

bool UdpSocket::isBound() const {
    return impl_->isBound();
}

bool UdpSocket::isReceiving() const {
    return impl_->isReceiving();
}

void UdpSocket::setDataReceivedCallback(const UdpDataReceivedCallback& callback) {
    dataReceivedCallback_ = callback;
    impl_->setCallbacks(dataReceivedCallback_, errorCallback_);
}

void UdpSocket::setErrorCallback(const UdpErrorCallback& callback) {
    errorCallback_ = callback;
    impl_->setCallbacks(dataReceivedCallback_, errorCallback_);
}

void UdpSocket::setReceiveBufferSize(size_t size) {
    impl_->setReceiveBufferSize(size);
}

void UdpSocket::setSendBufferSize(size_t size) {
    impl_->setSendBufferSize(size);
}

void UdpSocket::setBroadcast(bool enable) {
    impl_->setBroadcast(enable);
}

} // namespace FastNet
