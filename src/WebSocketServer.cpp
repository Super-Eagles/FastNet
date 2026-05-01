/**
 * @file WebSocketServer.cpp
 * @brief FastNet WebSocket 服务端实现
 */
#include "WebSocketServer.h"

#include "Error.h"
#include "HttpParser.h"
#include "PerformanceMonitor.h"
#include "TcpServer.h"
#include "Timer.h"
#include "WebSocketProtocol.h"

#include <algorithm>
#include <cctype>
#include <mutex>
#include <string_view>
#include <utility>
#include <vector>

namespace FastNet {

class WebSocketServer::Impl : public std::enable_shared_from_this<WebSocketServer::Impl> {
public:
    explicit Impl(IoService& ioService)
        : ioService_(ioService),
          tcpServer_(ioService) {}

    ~Impl() {
        stop();
    }

    Error start(uint16_t port, const std::string& bindAddress, const SSLConfig& sslConfig) {
        sslConfig_ = sslConfig;
        auto& performanceMonitor = getPerformanceMonitor();
        performanceMonitor.initialize();
        tcpServer_.setConnectionTimeout(connectionTimeout_);
        tcpServer_.setMaxConnections(maxConnections_);
        Error result = tcpServer_.start(port, bindAddress, sslConfig);
        if (result.isFailure()) {
            return result;
        }

        running_ = true;
        startPingTimer();
        return FASTNET_SUCCESS;
    }

    void stop() {
        if (!running_) {
            return;
        }

        running_ = false;
        if (pingTimer_) {
            pingTimer_->stop();
        }
        tcpServer_.stop();

        std::lock_guard<std::mutex> lock(clientStatesMutex_);
        clientStates_.clear();
        getPerformanceMonitor().setMetric("connections.active", 0);
    }

    Error sendTextToClient(ConnectionId clientId, const std::string& message) {
        return sendFrameToClient(clientId, WebSocketProtocol::encodeFrame(message, WSFrameType::Text, false));
    }

    Error sendBinaryToClient(ConnectionId clientId, const Buffer& data) {
        return sendFrameToClient(clientId, WebSocketProtocol::encodeFrame(data, WSFrameType::Binary, false));
    }

    void broadcastText(const std::string& message) {
        std::string frame = WebSocketProtocol::encodeFrame(message, WSFrameType::Text, false);
        tcpServer_.broadcast(std::move(frame));
    }

    void broadcastBinary(const Buffer& data) {
        std::string frame = WebSocketProtocol::encodeFrame(data, WSFrameType::Binary, false);
        tcpServer_.broadcast(std::move(frame));
    }

    void disconnectClient(ConnectionId clientId, uint16_t code, const std::string& reason) {
        bool alreadyClosing = false;
        bool directDisconnect = false;
        {
            std::lock_guard<std::mutex> lock(clientStatesMutex_);
            auto it = clientStates_.find(clientId);
            if (it == clientStates_.end()) {
                directDisconnect = true;
            } else if (it->second.status == ClientState::Status::Connected) {
                it->second.status = ClientState::Status::Closing;
            } else if (it->second.status == ClientState::Status::Closing) {
                alreadyClosing = true;
            } else {
                directDisconnect = true;
            }
        }

        if (directDisconnect) {
            tcpServer_.disconnectClient(clientId);
            return;
        }
        if (alreadyClosing) {
            tcpServer_.closeClientAfterPendingWrites(clientId);
            return;
        }

        std::string frame = encodeCloseFrame(code, reason);
        const Error sendResult = tcpServer_.sendToClient(clientId, std::move(frame));
        if (sendResult.isFailure()) {
            tcpServer_.disconnectClient(clientId);
            return;
        }
        tcpServer_.closeClientAfterPendingWrites(clientId);
    }

    size_t getClientCount() const { return tcpServer_.getClientCount(); }
    std::vector<ConnectionId> getClientIds() const { return tcpServer_.getClientIds(); }
    Address getClientAddress(ConnectionId clientId) const { return tcpServer_.getClientAddress(clientId); }
    void setClientConnectedCallback(const WebSocketServerClientConnectedCallback& callback) { clientConnectedCallback_ = callback; }
    void setClientDisconnectedCallback(const WebSocketServerClientDisconnectedCallback& callback) { clientDisconnectedCallback_ = callback; }
    void setMessageCallback(const WebSocketServerMessageCallback& callback) { messageCallback_ = callback; }
    void setBinaryCallback(const WebSocketServerBinaryCallback& callback) {
        binaryCallback_ = callback;
        ownedBinaryCallback_ = nullptr;
    }
    void setOwnedBinaryCallback(const WebSocketServerBinaryOwnedCallback& callback) {
        ownedBinaryCallback_ = callback;
        binaryCallback_ = nullptr;
    }
    void setServerErrorCallback(const WebSocketServerErrorCallback& callback) { serverErrorCallback_ = callback; }
    void setConnectionTimeout(uint32_t timeoutMs) {
        connectionTimeout_ = timeoutMs;
        tcpServer_.setConnectionTimeout(timeoutMs);
    }
    void setPingInterval(uint32_t intervalMs) {
        pingInterval_ = intervalMs;
        refreshPingTimer();
    }
    void setMaxConnections(size_t maxConnections) { maxConnections_ = maxConnections; tcpServer_.setMaxConnections(maxConnections); }
    Address getListenAddress() const { return tcpServer_.getListenAddress(); }
    bool isRunning() const { return running_; }

    void initializeCallbacks() {
        std::weak_ptr<Impl> weakSelf = shared_from_this();
        tcpServer_.setClientConnectedCallback([weakSelf](ConnectionId clientId, const Address& address) {
            auto self = weakSelf.lock();
            if (!self) {
                return;
            }
            self->handleClientConnected(clientId, address);
        });
        tcpServer_.setClientDisconnectedCallback([weakSelf](ConnectionId clientId, const std::string& reason) {
            auto self = weakSelf.lock();
            if (!self) {
                return;
            }
            self->handleClientDisconnected(clientId, reason);
        });
        tcpServer_.setOwnedDataReceivedCallback(
            [weakSelf](ConnectionId clientId, Buffer&& data) {
            auto self = weakSelf.lock();
            if (!self) {
                return;
            }
            self->handleDataReceived(clientId, std::move(data));
        });
        tcpServer_.setServerErrorCallback([weakSelf](const Error& error) {
            auto self = weakSelf.lock();
            if (!self) {
                return;
            }
            self->handleError(error.getCode(), error.getMessage());
        });
    }

private:
    struct ClientState {
        enum class Status {
            Handshake,
            Connected,
            Closing
        };

        Status status = Status::Handshake;
        Buffer buffer;
        size_t bufferOffset = 0;
        Address clientAddress;
        uint64_t lastActivity = 0;
    };

    void handleClientConnected(ConnectionId clientId, const Address& address) {
        bool accepted = false;
        {
            std::lock_guard<std::mutex> lock(clientStatesMutex_);
            if (clientStates_.size() >= maxConnections_) {
                clientStates_.erase(clientId);
            } else {
                ClientState state;
                state.status = ClientState::Status::Handshake;
                state.clientAddress = address;
                state.lastActivity = nowMs();
                clientStates_[clientId] = std::move(state);
                accepted = true;
            }
        }

        if (!accepted) {
            tcpServer_.disconnectClient(clientId);
            return;
        }

        auto& performanceMonitor = getPerformanceMonitor();
        performanceMonitor.incrementMetric("connections.total", 1);
        performanceMonitor.setMetric("connections.active", getClientCount());
    }

    void handleClientDisconnected(ConnectionId clientId, const std::string& reason) {
        bool notifyCallback = false;
        {
            std::lock_guard<std::mutex> lock(clientStatesMutex_);
            auto it = clientStates_.find(clientId);
            if (it != clientStates_.end()) {
                notifyCallback = it->second.status != ClientState::Status::Handshake;
                clientStates_.erase(it);
            }
        }

        getPerformanceMonitor().setMetric("connections.active", getClientCount());
        if (notifyCallback && clientDisconnectedCallback_) {
            clientDisconnectedCallback_(clientId, 1000, reason.empty() ? "Normal closure" : reason);
        }
    }

    void handleDataReceived(ConnectionId clientId, Buffer data) {
        ClientState::Status status;
        const size_t receivedBytes = data.size();
        {
            std::lock_guard<std::mutex> lock(clientStatesMutex_);
            auto it = clientStates_.find(clientId);
            if (it == clientStates_.end()) {
                return;
            }
            if (it->second.bufferOffset == 0 && it->second.buffer.empty()) {
                it->second.buffer = std::move(data);
            } else {
                it->second.buffer.insert(it->second.buffer.end(), data.begin(), data.end());
            }
            it->second.lastActivity = nowMs();
            status = it->second.status;
        }

        getPerformanceMonitor().incrementMetric("bytes.received", receivedBytes);
        if (status == ClientState::Status::Handshake) {
            handleHandshake(clientId);
        } else if (status == ClientState::Status::Connected) {
            handleFrames(clientId);
        }
    }

    void handleHandshake(ConnectionId clientId) {
        Address clientAddress;
        std::string upgradeValue;
        std::string connectionValue;
        std::string keyValue;
        std::string versionValue;
        size_t consumedBytes = 0;
        {
            std::lock_guard<std::mutex> lock(clientStatesMutex_);
            auto it = clientStates_.find(clientId);
            if (it == clientStates_.end()) {
                return;
            }
            clientAddress = it->second.clientAddress;

            const std::string_view raw = makeBufferedView(it->second);
            const size_t headerEnd = raw.find("\r\n\r\n");
            if (headerEnd == std::string_view::npos) {
                return;
            }
            consumedBytes = headerEnd + 4;

            HttpRequestView requestView;
            if (!HttpParser::parseRequest(raw.substr(0, headerEnd + 4), requestView)) {
                consumedBytes = 0;
            } else {
                const auto upgrade = requestView.getHeader("Upgrade");
                const auto connection = requestView.getHeader("Connection");
                const auto key = requestView.getHeader("Sec-WebSocket-Key");
                const auto version = requestView.getHeader("Sec-WebSocket-Version");
                if (upgrade.has_value()) {
                    upgradeValue.assign(upgrade->begin(), upgrade->end());
                }
                if (connection.has_value()) {
                    connectionValue.assign(connection->begin(), connection->end());
                }
                if (key.has_value()) {
                    keyValue.assign(key->begin(), key->end());
                }
                if (version.has_value()) {
                    versionValue.assign(version->begin(), version->end());
                }
            }
        }

        if (consumedBytes == 0) {
            handleError(ErrorCode::WebSocketHandshakeError, "Invalid WebSocket handshake request");
            disconnectClient(clientId, 1002, "Protocol error");
            return;
        }

        if (upgradeValue.empty() ||
            connectionValue.empty() ||
            keyValue.empty() ||
            versionValue.empty() ||
            !HttpParser::caseInsensitiveCompare(upgradeValue, "websocket") ||
            !containsToken(connectionValue, "upgrade") ||
            versionValue != "13") {
            handleError(ErrorCode::WebSocketHandshakeError, "Unsupported WebSocket handshake");
            disconnectClient(clientId, 1002, "Protocol error");
            return;
        }

        const uint64_t timerId = getPerformanceMonitor().startTimer();
        std::string response = WebSocketProtocol::createHandshakeResponse(keyValue);
        const size_t responseSize = response.size();
        if (!tcpServer_.sendToClient(clientId, std::move(response)).isSuccess()) {
            handleError(ErrorCode::WebSocketConnectionError, "Failed to send handshake response");
            disconnectClient(clientId, 1011, "Handshake failed");
            return;
        }
        getPerformanceMonitor().endTimer("handshake.time", timerId);
        getPerformanceMonitor().incrementMetric("bytes.sent", responseSize);

        bool hasRemaining = false;
        {
            std::lock_guard<std::mutex> lock(clientStatesMutex_);
            auto it = clientStates_.find(clientId);
            if (it == clientStates_.end()) {
                return;
            }
            it->second.status = ClientState::Status::Connected;
            it->second.bufferOffset += consumedBytes;
            compactBufferedStateLocked(it->second);
            hasRemaining = !makeBufferedView(it->second).empty();
        }

        if (clientConnectedCallback_) {
            clientConnectedCallback_(clientId, clientAddress);
        }

        if (hasRemaining) {
            handleFrames(clientId);
        }
    }

    void handleFrames(ConnectionId clientId) {
        while (true) {
            Buffer payload;
            WSFrameMetadata metadata;
            bool invalidFrame = false;
            bool frameTooLarge = false;
            {
                std::lock_guard<std::mutex> lock(clientStatesMutex_);
                auto it = clientStates_.find(clientId);
                if (it == clientStates_.end() || it->second.status != ClientState::Status::Connected) {
                    return;
                }

                const std::string_view pending = makeBufferedView(it->second);
                if (pending.size() > kMaxClientBufferSize) {
                    frameTooLarge = true;
                } else {
                    const size_t frameSize = getFrameSize(pending);
                    if (frameSize == 0 || frameSize > pending.size()) {
                        return;
                    }

                    invalidFrame =
                        !WebSocketProtocol::decodeFrame(pending.substr(0, frameSize), payload, metadata) || !metadata.final;
                    if (!invalidFrame) {
                        it->second.bufferOffset += frameSize;
                        compactBufferedStateLocked(it->second);
                    }
                }
            }

            if (frameTooLarge) {
                handleError(ErrorCode::WebSocketPayloadTooLarge, "Client buffer overflow");
                disconnectClient(clientId, 1009, "Message too big");
                return;
            }
            if (invalidFrame) {
                handleError(ErrorCode::WebSocketFrameError, "Invalid WebSocket frame");
                disconnectClient(clientId, 1002, "Protocol error");
                return;
            }

            const uint64_t timerId = getPerformanceMonitor().startTimer();
            handleFrame(clientId, metadata.type, std::move(payload));
            getPerformanceMonitor().endTimer("message.processing.time", timerId);
        }
    }

    void handleFrame(ConnectionId clientId, WSFrameType frameType, Buffer&& payload) {
        switch (frameType) {
            case WSFrameType::Text:
                getPerformanceMonitor().incrementMetric("messages.received", 1);
                if (messageCallback_) {
                    messageCallback_(clientId, std::string(reinterpret_cast<const char*>(payload.data()), payload.size()));
                }
                break;
            case WSFrameType::Binary:
                getPerformanceMonitor().incrementMetric("messages.received", 1);
                if (ownedBinaryCallback_) {
                    ownedBinaryCallback_(clientId, std::move(payload));
                } else if (binaryCallback_) {
                    binaryCallback_(clientId, payload);
                }
                break;
            case WSFrameType::Ping:
                sendFrameToClient(clientId, WebSocketProtocol::encodeFrame(payload, WSFrameType::Pong, false));
                break;
            case WSFrameType::Pong:
                updateLastActivity(clientId);
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
                disconnectClient(clientId, code, reason);
                break;
            }
            default:
                handleError(ErrorCode::WebSocketFrameError, "Unknown WebSocket frame type");
                disconnectClient(clientId, 1002, "Protocol error");
                break;
        }
    }

    Error sendFrameToClient(ConnectionId clientId, const std::string& frame) {
        return sendFrameToClient(clientId, std::string(frame));
    }

    Error sendFrameToClient(ConnectionId clientId, std::string&& frame) {
        const size_t frameSize = frame.size();
        Error result = tcpServer_.sendToClient(clientId, std::move(frame));
        if (result.isSuccess()) {
            getPerformanceMonitor().incrementMetric("messages.sent", 1);
            getPerformanceMonitor().incrementMetric("bytes.sent", frameSize);
        }
        return result;
    }

    std::string encodeCloseFrame(uint16_t code, const std::string& reason) const {
        std::string payload;
        payload.push_back(static_cast<char>((code >> 8) & 0xFF));
        payload.push_back(static_cast<char>(code & 0xFF));
        payload += reason;
        return WebSocketProtocol::encodeFrame(payload, WSFrameType::Close, false);
    }

    size_t getFrameSize(std::string_view data) const {
        if (data.size() < 2) {
            return 0;
        }

        size_t headerSize = 2;
        std::uint64_t payloadLength = static_cast<std::uint8_t>(data[1]) & 0x7F;
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
            payloadLength = 0;
            for (int i = 0; i < 8; ++i) {
                payloadLength = (payloadLength << 8) | static_cast<std::uint8_t>(data[2 + i]);
            }
            headerSize += 8;
        }

        if ((static_cast<std::uint8_t>(data[1]) & 0x80) != 0) {
            headerSize += 4;
        }

        if (data.size() < headerSize + payloadLength) {
            return 0;
        }
        return headerSize + static_cast<size_t>(payloadLength);
    }

    static std::string_view makeBufferedView(const ClientState& state) {
        if (state.bufferOffset >= state.buffer.size()) {
            return {};
        }
        return std::string_view(
            reinterpret_cast<const char*>(state.buffer.data() + state.bufferOffset),
            state.buffer.size() - state.bufferOffset);
    }

    static void compactBufferedStateLocked(ClientState& state) {
        if (state.bufferOffset == 0) {
            return;
        }
        if (state.bufferOffset >= state.buffer.size()) {
            state.buffer.clear();
            state.bufferOffset = 0;
            return;
        }
        if (state.bufferOffset < 4096 && state.bufferOffset * 2 < state.buffer.size()) {
            return;
        }
        state.buffer.erase(
            state.buffer.begin(),
            state.buffer.begin() + static_cast<std::ptrdiff_t>(state.bufferOffset));
        state.bufferOffset = 0;
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
            self->sendPingToAllClients();
        }, true);
    }

    void refreshPingTimer() {
        if (!running_) {
            return;
        }
        if (pingTimer_) {
            pingTimer_->stop();
        }
        if (pingInterval_ != 0) {
            startPingTimer();
        }
    }

    void sendPingToAllClients() {
        const uint64_t now = nowMs();
        std::vector<ConnectionId> timeoutClients;
        std::vector<ConnectionId> pingClients;

        {
            std::lock_guard<std::mutex> lock(clientStatesMutex_);
            for (const auto& entry : clientStates_) {
                if (entry.second.status != ClientState::Status::Connected) {
                    continue;
                }
                if (connectionTimeout_ != 0 && now - entry.second.lastActivity > connectionTimeout_) {
                    timeoutClients.push_back(entry.first);
                } else {
                    pingClients.push_back(entry.first);
                }
            }
        }

        for (ConnectionId clientId : pingClients) {
            sendFrameToClient(clientId, WebSocketProtocol::encodeFrame("", WSFrameType::Ping, false));
        }
        for (ConnectionId clientId : timeoutClients) {
            disconnectClient(clientId, 1001, "Connection timeout");
        }
    }

    void updateLastActivity(ConnectionId clientId) {
        std::lock_guard<std::mutex> lock(clientStatesMutex_);
        auto it = clientStates_.find(clientId);
        if (it != clientStates_.end()) {
            it->second.lastActivity = nowMs();
        }
    }

    void handleError(ErrorCode code, const std::string& errorMessage) const {
        getPerformanceMonitor().incrementMetric("errors.total", 1);
        if (serverErrorCallback_) {
            serverErrorCallback_(Error(code, errorMessage));
        }
    }

    static bool containsToken(std::string_view headerValue, std::string_view token) {
        auto toLowerAscii = [](char c) noexcept {
            return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
        };
        auto trimAscii = [](std::string_view input) noexcept {
            size_t begin = 0;
            size_t end = input.size();
            while (begin < end && std::isspace(static_cast<unsigned char>(input[begin])) != 0) {
                ++begin;
            }
            while (end > begin && std::isspace(static_cast<unsigned char>(input[end - 1])) != 0) {
                --end;
            }
            return input.substr(begin, end - begin);
        };
        auto equalsIgnoreCase = [&](std::string_view left, std::string_view right) noexcept {
            if (left.size() != right.size()) {
                return false;
            }
            for (size_t i = 0; i < left.size(); ++i) {
                if (toLowerAscii(left[i]) != toLowerAscii(right[i])) {
                    return false;
                }
            }
            return true;
        };

        size_t offset = 0;
        while (offset <= headerValue.size()) {
            const size_t separator = headerValue.find(',', offset);
            const std::string_view candidate = trimAscii(
                headerValue.substr(offset, separator == std::string_view::npos ? std::string_view::npos : separator - offset));
            if (equalsIgnoreCase(candidate, token)) {
                return true;
            }
            if (separator == std::string_view::npos) {
                break;
            }
            offset = separator + 1;
        }
        return false;
    }

    static uint64_t nowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    static constexpr size_t kMaxClientBufferSize = 10 * 1024 * 1024;

    IoService& ioService_;
    TcpServer tcpServer_;
    bool running_ = false;
    SSLConfig sslConfig_;
    uint32_t connectionTimeout_ = 30000;
    uint32_t pingInterval_ = 30000;
    size_t maxConnections_ = 10000;
    mutable std::mutex clientStatesMutex_;
    std::map<ConnectionId, ClientState> clientStates_;
    std::unique_ptr<Timer> pingTimer_;
    WebSocketServerClientConnectedCallback clientConnectedCallback_;
    WebSocketServerClientDisconnectedCallback clientDisconnectedCallback_;
    WebSocketServerMessageCallback messageCallback_;
    WebSocketServerBinaryCallback binaryCallback_;
    WebSocketServerBinaryOwnedCallback ownedBinaryCallback_;
    WebSocketServerErrorCallback serverErrorCallback_;
};

WebSocketServer::WebSocketServer(IoService& ioService)
    : impl_(std::make_shared<Impl>(ioService)) {
    impl_->initializeCallbacks();
}

WebSocketServer::~WebSocketServer() = default;

Error WebSocketServer::start(uint16_t port, const std::string& bindAddress, const SSLConfig& sslConfig) {
    return impl_->start(port, bindAddress, sslConfig);
}

void WebSocketServer::stop() {
    impl_->stop();
}

Error WebSocketServer::sendTextToClient(ConnectionId clientId, const std::string& message) {
    return impl_->sendTextToClient(clientId, message);
}

Error WebSocketServer::sendBinaryToClient(ConnectionId clientId, const Buffer& data) {
    return impl_->sendBinaryToClient(clientId, data);
}

void WebSocketServer::broadcastText(const std::string& message) {
    impl_->broadcastText(message);
}

void WebSocketServer::broadcastBinary(const Buffer& data) {
    impl_->broadcastBinary(data);
}

void WebSocketServer::disconnectClient(ConnectionId clientId, uint16_t code, const std::string& reason) {
    impl_->disconnectClient(clientId, code, reason);
}

size_t WebSocketServer::getClientCount() const {
    return impl_->getClientCount();
}

std::vector<ConnectionId> WebSocketServer::getClientIds() const {
    return impl_->getClientIds();
}

Address WebSocketServer::getClientAddress(ConnectionId clientId) const {
    return impl_->getClientAddress(clientId);
}

void WebSocketServer::setClientConnectedCallback(const WebSocketServerClientConnectedCallback& callback) {
    impl_->setClientConnectedCallback(callback);
}

void WebSocketServer::setClientDisconnectedCallback(const WebSocketServerClientDisconnectedCallback& callback) {
    impl_->setClientDisconnectedCallback(callback);
}

void WebSocketServer::setMessageCallback(const WebSocketServerMessageCallback& callback) {
    impl_->setMessageCallback(callback);
}

void WebSocketServer::setBinaryCallback(const WebSocketServerBinaryCallback& callback) {
    impl_->setBinaryCallback(callback);
}

void WebSocketServer::setOwnedBinaryCallback(const WebSocketServerBinaryOwnedCallback& callback) {
    impl_->setOwnedBinaryCallback(callback);
}

void WebSocketServer::setServerErrorCallback(const WebSocketServerErrorCallback& callback) {
    impl_->setServerErrorCallback(callback);
}

void WebSocketServer::setConnectionTimeout(uint32_t timeoutMs) {
    impl_->setConnectionTimeout(timeoutMs);
}

void WebSocketServer::setPingInterval(uint32_t intervalMs) {
    impl_->setPingInterval(intervalMs);
}

void WebSocketServer::setMaxConnections(size_t maxConnections) {
    impl_->setMaxConnections(maxConnections);
}

Address WebSocketServer::getListenAddress() const {
    return impl_->getListenAddress();
}

bool WebSocketServer::isRunning() const {
    return impl_->isRunning();
}

} // namespace FastNet
