/**
 * @file WebSocketClient.cpp
 * @brief FastNet WebSocket 客户端实现
 */
#include "WebSocketClient.h"

#include "Error.h"
#include "HttpParser.h"
#include "TcpClient.h"
#include "Timer.h"
#include "WebSocketProtocol.h"

#include <algorithm>
#include <cctype>
#include <utility>

namespace FastNet {

class WebSocketClient::Impl : public std::enable_shared_from_this<WebSocketClient::Impl> {
public:
    explicit Impl(IoService& ioService)
        : ioService_(ioService),
          tcpClient_(ioService) {}

    ~Impl() {
        stopTimers();
        tcpClient_.disconnect();
    }

    bool connect(const std::string& url, const WebSocketConnectCallback& callback) {
        pendingConnectCallback_ = callback;
        closeNotified_ = false;
        closeFrameSent_ = false;
        buffer_.clear();
        bufferOffset_ = 0;
        state_ = State::Disconnected;
        connected_ = false;

        if (!parseUrl(url)) {
            notifyConnect(false, "Invalid WebSocket URL");
            return false;
        }

        std::weak_ptr<Impl> weakSelf = shared_from_this();
        return tcpClient_.connect(
            serverAddress_,
            serverPort_,
            [weakSelf](bool success, const std::string& errorMessage) {
                auto self = weakSelf.lock();
                if (!self) {
                    return;
                }
                if (!success) {
                    self->notifyConnect(false, errorMessage);
                    return;
                }
                self->handleTcpConnected();
            },
            sslConfig_);
    }

    bool sendText(const std::string& message) {
        if (!connected_ || state_ != State::Connected) {
            return false;
        }

        std::string frame = WebSocketProtocol::encodeFrame(message, WSFrameType::Text, true);
        return tcpClient_.send(std::move(frame));
    }

    bool sendBinary(const Buffer& data) {
        if (!connected_ || state_ != State::Connected) {
            return false;
        }

        std::string frame = WebSocketProtocol::encodeFrame(data, WSFrameType::Binary, true);
        return tcpClient_.send(std::move(frame));
    }

    void close(uint16_t code, const std::string& reason) {
        if (state_ == State::Disconnected) {
            stopTimers();
            tcpClient_.disconnect();
            return;
        }
        if (state_ == State::Closing) {
            if (connected_) {
                startCloseTimer();
            }
            return;
        }

        state_ = State::Closing;
        stopTimers();
        if (connected_) {
            std::string frame = encodeCloseFrame(code, reason);
            closeFrameSent_ = true;
            if (!tcpClient_.send(std::move(frame))) {
                tcpClient_.disconnect();
                return;
            }
            startCloseTimer();
            return;
        }

        tcpClient_.disconnect();
    }

    void setConnectCallback(const WebSocketConnectCallback& callback) { connectCallback_ = callback; }
    void setMessageCallback(const WebSocketMessageCallback& callback) { messageCallback_ = callback; }
    void setBinaryCallback(const WebSocketBinaryCallback& callback) {
        binaryCallback_ = callback;
        ownedBinaryCallback_ = nullptr;
    }
    void setOwnedBinaryCallback(const WebSocketBinaryOwnedCallback& callback) {
        ownedBinaryCallback_ = callback;
        binaryCallback_ = nullptr;
    }
    void setErrorCallback(const WebSocketErrorCallback& callback) { errorCallback_ = callback; }
    void setCloseCallback(const WebSocketCloseCallback& callback) { closeCallback_ = callback; }
    void setConnectTimeout(uint32_t timeoutMs) { connectionTimeout_ = timeoutMs; }
    void setPingInterval(uint32_t intervalMs) { pingInterval_ = intervalMs; }
    void setSSLConfig(const SSLConfig& sslConfig) { sslConfig_ = sslConfig; }

    bool isConnected() const { return connected_; }
    Address getLocalAddress() const { return tcpClient_.getLocalAddress(); }
    Address getRemoteAddress() const { return tcpClient_.getRemoteAddress(); }
    void initializeCallbacks() {
        std::weak_ptr<Impl> weakSelf = shared_from_this();
        tcpClient_.setOwnedDataReceivedCallback([weakSelf](Buffer&& data) {
            auto self = weakSelf.lock();
            if (!self) {
                return;
            }
            self->handleDataReceived(std::move(data));
        });
        tcpClient_.setDisconnectCallback([weakSelf](const std::string& reason) {
            auto self = weakSelf.lock();
            if (!self) {
                return;
            }
            self->handleDisconnected(reason);
        });
        tcpClient_.setErrorCallback([weakSelf](ErrorCode code, const std::string& errorMessage) {
            auto self = weakSelf.lock();
            if (!self) {
                return;
            }
            self->handleError(Error(code, errorMessage));
        });
    }

private:
    enum class State {
        Disconnected,
        Handshake,
        Connected,
        Closing
    };

    void notifyConnect(bool success, const std::string& message) {
        if (pendingConnectCallback_) {
            auto callback = std::exchange(pendingConnectCallback_, nullptr);
            callback(success, message);
        }
        if (connectCallback_) {
            connectCallback_(success, message);
        }
    }

    void notifyClose(uint16_t code, const std::string& reason) {
        if (closeNotified_) {
            return;
        }
        closeNotified_ = true;
        if (closeCallback_) {
            closeCallback_(code, reason);
        }
    }

    bool parseUrl(const std::string& url) {
        sslConfig_.enableSSL = false;
        if (url.rfind("ws://", 0) == 0) {
            return parseUrlWithoutScheme(url.substr(5));
        }
        if (url.rfind("wss://", 0) == 0) {
            sslConfig_.enableSSL = true;
            return parseUrlWithoutScheme(url.substr(6));
        }
        return false;
    }

    bool parseUrlWithoutScheme(const std::string& url) {
        const size_t pathPos = url.find_first_of("/?");
        std::string authority = pathPos == std::string::npos ? url : url.substr(0, pathPos);
        resourcePath_ = pathPos == std::string::npos ? "/" : url.substr(pathPos);
        if (!resourcePath_.empty() && resourcePath_.front() == '?') {
            resourcePath_.insert(resourcePath_.begin(), '/');
        }

        if (authority.empty()) {
            return false;
        }

        serverPort_ = sslConfig_.enableSSL ? 443 : 80;
        if (!authority.empty() && authority.front() == '[') {
            const size_t closingBracket = authority.find(']');
            if (closingBracket == std::string::npos) {
                return false;
            }
            serverAddress_ = authority.substr(1, closingBracket - 1);
            if (closingBracket + 1 < authority.size()) {
                if (authority[closingBracket + 1] != ':') {
                    return false;
                }
                try {
                    const unsigned long parsedPort = std::stoul(authority.substr(closingBracket + 2));
                    if (parsedPort > 65535UL) {
                        return false;
                    }
                    serverPort_ = static_cast<uint16_t>(parsedPort);
                } catch (...) {
                    return false;
                }
            }
            return !serverAddress_.empty();
        }

        const size_t firstColon = authority.find(':');
        const size_t lastColon = authority.rfind(':');
        if (firstColon != std::string::npos && firstColon == lastColon) {
            serverAddress_ = authority.substr(0, firstColon);
            try {
                const unsigned long parsedPort = std::stoul(authority.substr(firstColon + 1));
                if (parsedPort > 65535UL) {
                    return false;
                }
                serverPort_ = static_cast<uint16_t>(parsedPort);
            } catch (...) {
                return false;
            }
        } else {
            serverAddress_ = authority;
        }

        return !serverAddress_.empty();
    }

    void handleTcpConnected() {
        connected_ = false;
        state_ = State::Handshake;
        buffer_.clear();
        bufferOffset_ = 0;
        handshakeKey_ = WebSocketProtocol::createHandshakeKey();
        if (!sendHandshakeRequest()) {
            notifyConnect(false, "Failed to send WebSocket handshake request");
            tcpClient_.disconnect();
            return;
        }
        startHandshakeTimer();
    }

    void handleDisconnected(const std::string& reason) {
        const bool shouldNotifyClose = connected_ || state_ == State::Closing;
        stopTimers();
        connected_ = false;
        closeFrameSent_ = false;
        state_ = State::Disconnected;
        buffer_.clear();
        bufferOffset_ = 0;
        if (shouldNotifyClose) {
            notifyClose(1000, reason.empty() ? "Normal closure" : reason);
        }
    }

    void handleDataReceived(Buffer data) {
        if (bufferOffset_ == 0 && buffer_.empty()) {
            buffer_ = std::move(data);
        } else {
            buffer_.insert(buffer_.end(), data.begin(), data.end());
        }
        if (state_ == State::Handshake) {
            handleHandshakeResponse();
            return;
        }
        if (state_ == State::Connected || state_ == State::Closing) {
            handleFrames();
        }
    }

    void handleError(const Error& error) {
        if (errorCallback_) {
            errorCallback_(error.getCode(), error.getMessage());
        }
    }

    bool sendHandshakeRequest() {
        std::string request;
        request.reserve(resourcePath_.size() + serverAddress_.size() + handshakeKey_.size() + 128);
        request += "GET ";
        request += resourcePath_;
        request += " HTTP/1.1\r\nHost: ";
        request += buildHostHeader();
        request += "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ";
        request += handshakeKey_;
        request += "\r\nSec-WebSocket-Version: 13\r\nUser-Agent: FastNet-WebSocketClient/2.0\r\n\r\n";
        return tcpClient_.send(std::move(request));
    }

    std::string buildHostHeader() const {
        const bool defaultPort = (!sslConfig_.enableSSL && serverPort_ == 80) ||
                                 (sslConfig_.enableSSL && serverPort_ == 443);
        const std::string renderedHost =
            Address::isValidIPv6(serverAddress_) ? ("[" + serverAddress_ + "]") : serverAddress_;
        if (defaultPort) {
            return renderedHost;
        }
        return renderedHost + ":" + std::to_string(serverPort_);
    }

    void handleHandshakeResponse() {
        const std::string_view raw = makeBufferedView();
        const size_t headerEnd = raw.find("\r\n\r\n");
        if (headerEnd == std::string_view::npos) {
            return;
        }

        HttpResponseView responseView;
        if (!HttpParser::parseResponse(raw.substr(0, headerEnd + 4), responseView)) {
            notifyConnect(false, "Invalid WebSocket handshake response");
            tcpClient_.disconnect();
            return;
        }

        const auto upgrade = responseView.getHeader("Upgrade");
        const auto connection = responseView.getHeader("Connection");
        const auto accept = responseView.getHeader("Sec-WebSocket-Accept");
        if (responseView.statusCode != 101 ||
            !upgrade.has_value() ||
            !connection.has_value() ||
            !accept.has_value()) {
            notifyConnect(false, "WebSocket handshake failed");
            tcpClient_.disconnect();
            return;
        }

        std::string connectionValue(connection->begin(), connection->end());
        std::transform(connectionValue.begin(), connectionValue.end(), connectionValue.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });

        if (!HttpParser::caseInsensitiveCompare(*upgrade, "websocket") ||
            connectionValue.find("upgrade") == std::string::npos ||
            std::string(accept->begin(), accept->end()) != WebSocketProtocol::createAcceptKey(handshakeKey_)) {
            notifyConnect(false, "WebSocket handshake validation failed");
            tcpClient_.disconnect();
            return;
        }

        if (handshakeTimer_) {
            handshakeTimer_->stop();
        }

        bufferOffset_ += headerEnd + 4;
        compactBuffer();
        state_ = State::Connected;
        connected_ = true;
        lastActivity_ = getCurrentTimestamp();
        startPingTimer();
        notifyConnect(true, "");

        if (!makeBufferedView().empty()) {
            handleFrames();
        }
    }

    void handleFrames() {
        while (true) {
            const std::string_view pending = makeBufferedView();
            const size_t frameSize = getFrameSize(pending);
            if (frameSize == 0 || frameSize > pending.size()) {
                return;
            }

            Buffer payload;
            WSFrameMetadata metadata;
            if (!WebSocketProtocol::decodeFrame(pending.substr(0, frameSize), payload, metadata) || !metadata.final) {
                handleError(Error(ErrorCode::WebSocketFrameError, "Invalid WebSocket frame"));
                tcpClient_.disconnect();
                return;
            }

            bufferOffset_ += frameSize;
            compactBuffer();
            handleFrame(metadata.type, std::move(payload));
        }
    }

    void handleFrame(WSFrameType frameType, Buffer&& payload) {
        lastActivity_ = getCurrentTimestamp();

        switch (frameType) {
            case WSFrameType::Text:
                if (state_ == State::Connected && messageCallback_) {
                    messageCallback_(std::string(reinterpret_cast<const char*>(payload.data()), payload.size()));
                }
                break;
            case WSFrameType::Binary:
                if (state_ == State::Connected) {
                    if (ownedBinaryCallback_) {
                        ownedBinaryCallback_(std::move(payload));
                    } else if (binaryCallback_) {
                        binaryCallback_(payload);
                    }
                }
                break;
            case WSFrameType::Close: {
                uint16_t code = 1000;
                std::string reason = "Normal closure";
                if (payload.size() >= 2) {
                    code = (static_cast<uint16_t>(payload[0]) << 8) |
                           static_cast<uint16_t>(payload[1]);
                    if (payload.size() > 2) {
                        reason.assign(reinterpret_cast<const char*>(payload.data() + 2), payload.size() - 2);
                    }
                }
                notifyClose(code, reason);
                if (!closeFrameSent_) {
                    std::string frame = encodeCloseFrame(code, reason);
                    closeFrameSent_ = true;
                    state_ = State::Closing;
                    stopTimers();
                    if (!tcpClient_.send(std::move(frame))) {
                        tcpClient_.disconnect();
                        break;
                    }
                }
                tcpClient_.disconnectAfterPendingWrites();
                break;
            }
            case WSFrameType::Ping:
                sendControlFrame(std::string(reinterpret_cast<const char*>(payload.data()), payload.size()), WSFrameType::Pong);
                break;
            case WSFrameType::Pong:
                break;
            default:
                break;
        }
    }

    void sendControlFrame(const std::string& payload, WSFrameType type) {
        std::string frame = WebSocketProtocol::encodeFrame(payload, type, true);
        tcpClient_.send(std::move(frame));
    }

    std::string encodeCloseFrame(uint16_t code, const std::string& reason) const {
        std::string payload;
        payload.push_back(static_cast<char>((code >> 8) & 0xFF));
        payload.push_back(static_cast<char>(code & 0xFF));
        payload += reason;
        return WebSocketProtocol::encodeFrame(payload, WSFrameType::Close, true);
    }

    size_t getFrameSize(std::string_view data) const {
        if (data.size() < 2) {
            return 0;
        }

        size_t headerSize = 2;
        const auto byte1 = static_cast<std::uint8_t>(data[1]);
        std::uint64_t payloadLength = byte1 & 0x7Fu;
        if (payloadLength == 126) {
            if (data.size() < 4) {
                return 0;
            }
            payloadLength = (static_cast<std::uint64_t>(static_cast<std::uint8_t>(data[2])) << 8) |
                             static_cast<std::uint64_t>(static_cast<std::uint8_t>(data[3]));
            headerSize += 2;
        } else if (payloadLength == 127) {
            if (data.size() < 10) {
                return 0;
            }
            // Unroll 8-byte big-endian decode.
            payloadLength =
                (static_cast<std::uint64_t>(static_cast<std::uint8_t>(data[2])) << 56) |
                (static_cast<std::uint64_t>(static_cast<std::uint8_t>(data[3])) << 48) |
                (static_cast<std::uint64_t>(static_cast<std::uint8_t>(data[4])) << 40) |
                (static_cast<std::uint64_t>(static_cast<std::uint8_t>(data[5])) << 32) |
                (static_cast<std::uint64_t>(static_cast<std::uint8_t>(data[6])) << 24) |
                (static_cast<std::uint64_t>(static_cast<std::uint8_t>(data[7])) << 16) |
                (static_cast<std::uint64_t>(static_cast<std::uint8_t>(data[8])) <<  8) |
                 static_cast<std::uint64_t>(static_cast<std::uint8_t>(data[9]));
            headerSize += 8;
        }

        if ((byte1 & 0x80u) != 0) {
            headerSize += 4;
        }

        if (data.size() < headerSize + payloadLength) {
            return 0;
        }
        return headerSize + static_cast<size_t>(payloadLength);
    }

    std::string_view makeBufferedView() const {
        if (bufferOffset_ >= buffer_.size()) {
            return {};
        }
        return std::string_view(
            reinterpret_cast<const char*>(buffer_.data() + bufferOffset_),
            buffer_.size() - bufferOffset_);
    }

    void compactBuffer() {
        if (bufferOffset_ == 0) {
            return;
        }
        if (bufferOffset_ >= buffer_.size()) {
            buffer_.clear();
            bufferOffset_ = 0;
            return;
        }
        // Only compact when the wasted prefix exceeds 64 KB to avoid O(n) memmove
        // on every frame in a high-throughput stream.
        const size_t waste = bufferOffset_;
        if (waste < 65536 && waste * 4 < buffer_.size()) {
            return;
        }
        buffer_.erase(buffer_.begin(),
                      buffer_.begin() + static_cast<std::ptrdiff_t>(bufferOffset_));
        bufferOffset_ = 0;
    }

    void startHandshakeTimer() {
        if (!handshakeTimer_) {
            handshakeTimer_ = std::make_unique<Timer>(ioService_);
        }
        std::weak_ptr<Impl> weakSelf = shared_from_this();
        handshakeTimer_->start(std::chrono::milliseconds(connectionTimeout_), [weakSelf]() {
            auto self = weakSelf.lock();
            if (!self) {
                return;
            }
            self->handleHandshakeTimeout();
        });
    }

    void startPingTimer() {
        if (pingInterval_ == 0) {
            return;
        }
        if (!pingTimer_) {
            pingTimer_ = std::make_unique<Timer>(ioService_);
        }
        std::weak_ptr<Impl> weakSelf = shared_from_this();
        pingTimer_->start(std::chrono::milliseconds(pingInterval_), [weakSelf]() {
            auto self = weakSelf.lock();
            if (!self) {
                return;
            }
            self->sendPing();
        }, true);
    }

    void startCloseTimer() {
        if (!closeTimer_) {
            closeTimer_ = std::make_unique<Timer>(ioService_);
        }
        const uint32_t timeoutMs = connectionTimeout_ == 0 ? 5000 : connectionTimeout_;
        std::weak_ptr<Impl> weakSelf = shared_from_this();
        closeTimer_->start(std::chrono::milliseconds(timeoutMs), [weakSelf]() {
            auto self = weakSelf.lock();
            if (!self) {
                return;
            }
            if (self->state_ == State::Closing) {
                self->tcpClient_.disconnect();
            }
        });
    }

    void sendPing() {
        if (!connected_ || state_ != State::Connected) {
            return;
        }
        sendControlFrame("", WSFrameType::Ping);
    }

    void handleHandshakeTimeout() {
        if (state_ != State::Handshake || connected_) {
            return;
        }
        notifyConnect(false, "WebSocket handshake timeout");
        tcpClient_.disconnect();
    }

    void stopTimers() {
        if (pingTimer_) {
            pingTimer_->stop();
        }
        if (handshakeTimer_) {
            handshakeTimer_->stop();
        }
        if (closeTimer_) {
            closeTimer_->stop();
        }
    }

    static uint64_t getCurrentTimestamp() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    IoService& ioService_;
    TcpClient tcpClient_;
    bool connected_ = false;
    State state_ = State::Disconnected;
    bool closeNotified_ = false;
    SSLConfig sslConfig_;
    uint32_t connectionTimeout_ = 30000;
    uint32_t pingInterval_ = 30000;
    uint64_t lastActivity_ = 0;
    std::string serverAddress_;
    uint16_t serverPort_ = 0;
    std::string resourcePath_ = "/";
    std::string handshakeKey_;
    Buffer buffer_;
    size_t bufferOffset_ = 0;
    std::unique_ptr<Timer> pingTimer_;
    std::unique_ptr<Timer> handshakeTimer_;
    std::unique_ptr<Timer> closeTimer_;
    WebSocketConnectCallback connectCallback_;
    WebSocketConnectCallback pendingConnectCallback_;
    WebSocketMessageCallback messageCallback_;
    WebSocketBinaryCallback binaryCallback_;
    WebSocketBinaryOwnedCallback ownedBinaryCallback_;
    WebSocketErrorCallback errorCallback_;
    WebSocketCloseCallback closeCallback_;
    bool closeFrameSent_ = false;
};

WebSocketClient::WebSocketClient(IoService& ioService)
    : impl_(std::make_shared<Impl>(ioService)) {
    impl_->initializeCallbacks();
}

WebSocketClient::~WebSocketClient() = default;

bool WebSocketClient::connect(const std::string& url, const WebSocketConnectCallback& callback) {
    return impl_->connect(url, callback);
}

bool WebSocketClient::sendText(const std::string& message) {
    return impl_->sendText(message);
}

bool WebSocketClient::sendBinary(const Buffer& data) {
    return impl_->sendBinary(data);
}

void WebSocketClient::close(uint16_t code, const std::string& reason) {
    impl_->close(code, reason);
}

void WebSocketClient::setConnectCallback(const WebSocketConnectCallback& callback) {
    impl_->setConnectCallback(callback);
}

void WebSocketClient::setMessageCallback(const WebSocketMessageCallback& callback) {
    impl_->setMessageCallback(callback);
}

void WebSocketClient::setBinaryCallback(const WebSocketBinaryCallback& callback) {
    impl_->setBinaryCallback(callback);
}

void WebSocketClient::setOwnedBinaryCallback(const WebSocketBinaryOwnedCallback& callback) {
    impl_->setOwnedBinaryCallback(callback);
}

void WebSocketClient::setErrorCallback(const WebSocketErrorCallback& callback) {
    impl_->setErrorCallback(callback);
}

void WebSocketClient::setCloseCallback(const WebSocketCloseCallback& callback) {
    impl_->setCloseCallback(callback);
}

void WebSocketClient::setConnectTimeout(uint32_t timeoutMs) {
    impl_->setConnectTimeout(timeoutMs);
}

void WebSocketClient::setPingInterval(uint32_t intervalMs) {
    impl_->setPingInterval(intervalMs);
}

void WebSocketClient::setSSLConfig(const SSLConfig& sslConfig) {
    impl_->setSSLConfig(sslConfig);
}

bool WebSocketClient::isConnected() const {
    return impl_->isConnected();
}

Address WebSocketClient::getLocalAddress() const {
    return impl_->getLocalAddress();
}

Address WebSocketClient::getRemoteAddress() const {
    return impl_->getRemoteAddress();
}

} // namespace FastNet
