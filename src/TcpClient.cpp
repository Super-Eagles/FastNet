/**
 * @file TcpClient.cpp
 * @brief FastNet TCP client implementation
 */
#include "TcpClient.h"

#include "Error.h"
#include "MemoryPool.h"
#include "SSLContext.h"
#include "SocketWrapper.h"
#include "Timer.h"
#include "SharedSessionTypes.h"
#include "WindowsIocpTransport.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <utility>

#ifdef FASTNET_ENABLE_SSL
#include <openssl/err.h>
#include <openssl/x509_vfy.h>
#endif

namespace FastNet {

namespace {


} // namespace

class TcpClient::Impl : public std::enable_shared_from_this<TcpClient::Impl>
#ifdef _WIN32
                      , public WindowsIocpSocketHandler
#endif
{
public:
    explicit Impl(IoService& ioService)
        : ioService_(ioService),
          connectTimer_(std::make_unique<Timer>(ioService)),
          readTimer_(std::make_unique<Timer>(ioService)),
          writeTimer_(std::make_unique<Timer>(ioService)) {}

    ~Impl() {
        closeWithReason("Connection closed");
    }

    bool connect(const std::string& host,
                 uint16_t port,
                 const ConnectCallback& callback,
                 const SSLConfig& sslConfig) {
        pendingConnectCallback_ = callback;

        if (running_.load(std::memory_order_acquire)) {
            setLastFailure(ErrorCode::AlreadyRunning, "Connection is already active");
            notifyConnect(false, "Connection is already active");
            return false;
        }

        resetTransportState();
        sslConfig_ = sslConfig;
        const std::string normalizedHost(Address::stripBrackets(host));
        if (sslConfig_.enableSSL &&
            sslConfig_.hostnameVerification.empty() &&
            !normalizedHost.empty()) {
            sslConfig_.hostnameVerification = normalizedHost;
        }

        Error result = socket_.create(SocketType::TCP, normalizedHost, port);
        if (result.isFailure()) {
            setLastFailure(result.getCode(), result.getMessage());
            notifyConnect(false, result.getMessage());
            return false;
        }

        SocketOption option;
        option.noDelay = true;
        option.keepAlive = true;
        result = socket_.setOption(option);
        if (result.isFailure()) {
            socket_.close();
            setLastFailure(result.getCode(), result.getMessage());
            notifyConnect(false, result.getMessage());
            return false;
        }
        socket_.optimizeLoopbackFastPath();

        result = socket_.connect(normalizedHost, port, static_cast<int>(connectTimeout_));
        if (result.isFailure()) {
            socket_.close();
            setLastFailure(result.getCode(), result.getMessage());
            notifyConnect(false, result.getMessage());
            return false;
        }

        updateAddresses(host, port);

#ifdef _WIN32
        const bool useWindowsIocpTransport = true;
#else
        const bool useWindowsIocpTransport = false;
#endif

        if (useWindowsIocpTransport) {
#ifdef _WIN32
            running_.store(true, std::memory_order_release);
            windowsTransport_ = WindowsIocpSocketTransport::create(
                socket_.getFd(),
                std::static_pointer_cast<WindowsIocpSocketHandler>(shared_from_this()),
                ioService_.getThreadCount(),
                8192);
            if (!windowsTransport_) {
                socket_.close();
                running_.store(false, std::memory_order_release);
                setLastFailure(ErrorCode::SocketError, "Failed to create Windows IOCP transport");
                notifyConnect(false, "Failed to create Windows IOCP transport");
                return false;
            }

            result = windowsTransport_->start();
            if (result.isFailure()) {
                auto transport = std::move(windowsTransport_);
                if (transport) {
                    transport->stop();
                }
                socket_.close();
                running_.store(false, std::memory_order_release);
                setLastFailure(result.getCode(), result.toString());
                notifyConnect(false, result.toString());
                return false;
            }

            armConnectTimer();

            if (sslConfig_.enableSSL) {
                if (!initializeTls()) {
                    const Error lastError = getLastError();
                    notifyConnect(false, lastError.isFailure() ? lastError.toString() : "TLS initialization failed");
                    cleanupInactiveTransport();
                    return false;
                }

                if (!driveTlsHandshake()) {
                    const Error lastError = getLastError();
                    notifyConnect(false, lastError.isFailure() ? lastError.toString() : "TLS handshake failed");
                    cleanupInactiveTransport();
                    return false;
                }
                return true;
            }

            markConnected();
            return true;
#endif
        }

        result = socket_.setNonBlocking(true);
        if (result.isFailure()) {
            socket_.close();
            setLastFailure(result.getCode(), result.getMessage());
            notifyConnect(false, result.getMessage());
            return false;
        }

        if (!ioService_.getPoller().add(
                socket_.getFd(),
                EventType::Read | EventType::Error | EventType::Close,
                [weakSelf = std::weak_ptr<Impl>(shared_from_this())](socket_t, EventType events, void*, size_t) {
                    auto self = weakSelf.lock();
                    if (!self) {
                        return;
                    }
                    if (hasEvent(events, EventType::Read)) {
                        self->handleRead();
                    }
                    if (hasEvent(events, EventType::Write)) {
                        self->handleWrite();
                    }
                    if (hasEvent(events, EventType::Error) || hasEvent(events, EventType::Close)) {
                        self->handleTransportFailure(ErrorCode::SocketError, "Connection closed by peer");
                    }
                })) {
            socket_.close();
            setLastFailure(ErrorCode::SocketError, "Failed to register socket with poller");
            notifyConnect(false, "Failed to register socket with poller");
            return false;
        }

        running_.store(true, std::memory_order_release);
        armConnectTimer();

        if (sslConfig_.enableSSL) {
            if (!initializeTls()) {
                const Error lastError = getLastError();
                notifyConnect(false, lastError.isFailure() ? lastError.toString() : "TLS initialization failed");
                cleanupInactiveTransport();
                return false;
            }

            if (!driveTlsHandshake()) {
                const Error lastError = getLastError();
                notifyConnect(false, lastError.isFailure() ? lastError.toString() : "TLS handshake failed");
                cleanupInactiveTransport();
                return false;
            }

            return true;
        }

        markConnected();
        return true;
    }

    void disconnect() {
        if (running_.load(std::memory_order_acquire) &&
            !connected_.load(std::memory_order_acquire) &&
            pendingConnectCallback_) {
            notifyConnect(false, "Connection cancelled");
        }
        closeWithReason("Connection closed");
    }

    void disconnectAfterPendingWrites() {
        if (!connected_.load(std::memory_order_acquire)) {
            disconnect();
            return;
        }

        bool shouldCloseNow = false;
        {
            std::lock_guard<std::mutex> lock(sendMutex_);
            closeAfterFlush_ = true;
            bool hasPendingOutput = pendingWriteBytes_ != 0;
#ifdef FASTNET_ENABLE_SSL
            if (usingWindowsIocpTls()) {
                hasPendingOutput = hasPendingOutput || hasPendingWindowsIocpTlsOutputLocked();
            }
#endif
            if (!hasPendingOutput) {
                closeAfterFlush_ = false;
                shouldCloseNow = true;
            } else {
                refreshInterestLocked();
            }
        }

        if (shouldCloseNow) {
            closeWithReason("Connection closed");
        }
    }

    bool send(const Buffer& data) {
        std::lock_guard<std::recursive_mutex> callbackLock(callbackMutex_);
        if (!connected_.load(std::memory_order_acquire)) {
            setLastFailure(ErrorCode::ConnectionError, "Connection is not established");
            return false;
        }
        if (data.empty()) {
            return true;
        }

        bool shouldScheduleFlush = false;
        {
            std::lock_guard<std::mutex> lock(sendMutex_);
            QueuedWrite queuedWrite;
            queuedWrite.bufferStorage = data;
            sendQueue_.push_back(std::move(queuedWrite));
            pendingWriteBytes_ += data.size();
#ifdef _WIN32
            if (usingWindowsIocpTransport()) {
                shouldScheduleFlush = shouldStartWindowsIocpFlushLocked();
            } else
#endif
            {
                refreshInterestLocked();
                shouldScheduleFlush = markFlushScheduledLocked();
            }
        }

        armWriteTimer();
        dispatchFlushIfNeeded(shouldScheduleFlush);
        return true;
    }

    bool send(Buffer&& data) {
        std::lock_guard<std::recursive_mutex> callbackLock(callbackMutex_);
        if (!connected_.load(std::memory_order_acquire)) {
            setLastFailure(ErrorCode::ConnectionError, "Connection is not established");
            return false;
        }
        if (data.empty()) {
            return true;
        }

        const size_t dataSize = data.size();
        bool shouldScheduleFlush = false;
        {
            std::lock_guard<std::mutex> lock(sendMutex_);
            QueuedWrite queuedWrite;
            queuedWrite.bufferStorage = std::move(data);
            sendQueue_.push_back(std::move(queuedWrite));
            pendingWriteBytes_ += dataSize;
#ifdef _WIN32
            if (usingWindowsIocpTransport()) {
                shouldScheduleFlush = shouldStartWindowsIocpFlushLocked();
            } else
#endif
            {
                refreshInterestLocked();
                shouldScheduleFlush = markFlushScheduledLocked();
            }
        }

        armWriteTimer();
        dispatchFlushIfNeeded(shouldScheduleFlush);
        return true;
    }

    bool send(const std::shared_ptr<const Buffer>& data) {
        std::lock_guard<std::recursive_mutex> callbackLock(callbackMutex_);
        if (!connected_.load(std::memory_order_acquire)) {
            setLastFailure(ErrorCode::ConnectionError, "Connection is not established");
            return false;
        }
        if (!data || data->empty()) {
            return true;
        }

        bool shouldScheduleFlush = false;
        {
            std::lock_guard<std::mutex> lock(sendMutex_);
            QueuedWrite queuedWrite;
            queuedWrite.bufferPayload = data;
            sendQueue_.push_back(std::move(queuedWrite));
            pendingWriteBytes_ += data->size();
#ifdef _WIN32
            if (usingWindowsIocpTransport()) {
                shouldScheduleFlush = shouldStartWindowsIocpFlushLocked();
            } else
#endif
            {
                refreshInterestLocked();
                shouldScheduleFlush = markFlushScheduledLocked();
            }
        }

        armWriteTimer();
        dispatchFlushIfNeeded(shouldScheduleFlush);
        return true;
    }

    bool send(std::string&& data) {
        std::lock_guard<std::recursive_mutex> callbackLock(callbackMutex_);
        if (!connected_.load(std::memory_order_acquire)) {
            setLastFailure(ErrorCode::ConnectionError, "Connection is not established");
            return false;
        }
        if (data.empty()) {
            return true;
        }

        const size_t dataSize = data.size();
        bool shouldScheduleFlush = false;
        {
            std::lock_guard<std::mutex> lock(sendMutex_);
            QueuedWrite queuedWrite;
            queuedWrite.stringStorage = std::move(data);
            sendQueue_.push_back(std::move(queuedWrite));
            pendingWriteBytes_ += dataSize;
#ifdef _WIN32
            if (usingWindowsIocpTransport()) {
                shouldScheduleFlush = shouldStartWindowsIocpFlushLocked();
            } else
#endif
            {
                refreshInterestLocked();
                shouldScheduleFlush = markFlushScheduledLocked();
            }
        }

        armWriteTimer();
        dispatchFlushIfNeeded(shouldScheduleFlush);
        return true;
    }

    bool send(std::string_view data) {
        std::lock_guard<std::recursive_mutex> callbackLock(callbackMutex_);
        if (!connected_.load(std::memory_order_acquire)) {
            setLastFailure(ErrorCode::ConnectionError, "Connection is not established");
            return false;
        }
        if (data.empty()) {
            return true;
        }

        bool shouldScheduleFlush = false;
        {
            std::lock_guard<std::mutex> lock(sendMutex_);
            QueuedWrite queuedWrite;
            queuedWrite.stringStorage.assign(data.data(), data.size());
            sendQueue_.push_back(std::move(queuedWrite));
            pendingWriteBytes_ += data.size();
#ifdef _WIN32
            if (usingWindowsIocpTransport()) {
                shouldScheduleFlush = shouldStartWindowsIocpFlushLocked();
            } else
#endif
            {
                refreshInterestLocked();
                shouldScheduleFlush = markFlushScheduledLocked();
            }
        }

        armWriteTimer();
        dispatchFlushIfNeeded(shouldScheduleFlush);
        return true;
    }

    bool send(const std::shared_ptr<const std::string>& data) {
        std::lock_guard<std::recursive_mutex> callbackLock(callbackMutex_);
        if (!connected_.load(std::memory_order_acquire)) {
            setLastFailure(ErrorCode::ConnectionError, "Connection is not established");
            return false;
        }
        if (!data || data->empty()) {
            return true;
        }

        bool shouldScheduleFlush = false;
        {
            std::lock_guard<std::mutex> lock(sendMutex_);
            QueuedWrite queuedWrite;
            queuedWrite.stringPayload = data;
            sendQueue_.push_back(std::move(queuedWrite));
            pendingWriteBytes_ += data->size();
#ifdef _WIN32
            if (usingWindowsIocpTransport()) {
                shouldScheduleFlush = shouldStartWindowsIocpFlushLocked();
            } else
#endif
            {
                refreshInterestLocked();
                shouldScheduleFlush = markFlushScheduledLocked();
            }
        }

        armWriteTimer();
        dispatchFlushIfNeeded(shouldScheduleFlush);
        return true;
    }

    void setCallbacks(const ConnectCallback& connectCb,
                      const DisconnectCallback& disconnectCb,
                      const DataReceivedCallback& dataReceivedCb,
                      const OwnedDataReceivedCallback& ownedDataReceivedCb,
                      const SharedDataReceivedCallback& sharedDataReceivedCb,
                      const ErrorCallback& errorCb) {
        std::lock_guard<std::recursive_mutex> callbackLock(callbackMutex_);
        connectCallback_ = connectCb;
        disconnectCallback_ = disconnectCb;
        dataReceivedCallback_ = dataReceivedCb;
        ownedDataReceivedCallback_ = ownedDataReceivedCb;
        sharedDataReceivedCallback_ = sharedDataReceivedCb;
        errorCallback_ = errorCb;
    }

    Address getLocalAddress() const { return localAddress_; }
    Address getRemoteAddress() const { return remoteAddress_; }
    bool isConnected() const { return connected_.load(std::memory_order_acquire); }
    bool isSecure() const { return tlsState_ == TlsState::Ready; }
    Error getLastError() const {
        std::lock_guard<std::mutex> lock(failureMutex_);
        return lastFailureCode_ == ErrorCode::Success ? Error::success()
                                                      : Error(lastFailureCode_, lastFailureMessage_);
    }
    size_t getPendingWriteBytes() const {
        std::lock_guard<std::mutex> lock(sendMutex_);
        return pendingWriteBytes_;
    }
    void setConnectTimeout(uint32_t timeoutMs) {
        connectTimeout_ = timeoutMs;
        if (!running_.load(std::memory_order_acquire) || connected_.load(std::memory_order_acquire)) {
            stopConnectTimer();
            return;
        }
        if (connectTimeout_ == 0) {
            stopConnectTimer();
        } else {
            armConnectTimer();
        }
    }
    void setReadTimeout(uint32_t timeoutMs) {
        readTimeout_ = timeoutMs;
        if (!connected_.load(std::memory_order_acquire) || readTimeout_ == 0) {
            stopReadTimer();
        } else {
            armReadTimer();
        }
    }
    void setWriteTimeout(uint32_t timeoutMs) {
        writeTimeout_ = timeoutMs;
        bool hasPendingWrite = false;
        {
            std::lock_guard<std::mutex> lock(sendMutex_);
            hasPendingWrite = !sendQueue_.empty();
#ifdef FASTNET_ENABLE_SSL
            if (usingWindowsIocpTls()) {
                hasPendingWrite = hasPendingWrite || hasPendingWindowsIocpTlsOutputLocked();
            }
#endif
        }
        if (!connected_.load(std::memory_order_acquire) || writeTimeout_ == 0 || !hasPendingWrite) {
            stopWriteTimer();
        } else {
            armWriteTimer();
        }
    }

private:
    void clearLastFailure() {
        std::lock_guard<std::mutex> lock(failureMutex_);
        lastFailureCode_ = ErrorCode::Success;
        lastFailureMessage_.clear();
    }

#ifdef _WIN32
    void retainOutstandingWindowsIocpWriteBuffersLocked() {
        if (!iocpWritePending_) {
            return;
        }
        if (!sendQueue_.empty()) {
            retainedSendBatches_.push_back(std::make_unique<std::deque<QueuedWrite>>(std::move(sendQueue_)));
        }
#ifdef FASTNET_ENABLE_SSL
        if (!tlsWriteQueue_.empty()) {
            retainedTlsWriteBatches_.push_back(std::make_unique<std::deque<Buffer>>(std::move(tlsWriteQueue_)));
        }
#endif
    }

    void releaseRetainedWindowsIocpWriteBuffersLocked() {
        if (iocpWritePending_) {
            return;
        }
        retainedSendBatches_.clear();
#ifdef FASTNET_ENABLE_SSL
        retainedTlsWriteBatches_.clear();
#endif
    }
#endif

    void setLastFailure(ErrorCode code, std::string message) {
        std::lock_guard<std::mutex> lock(failureMutex_);
        lastFailureCode_ = code;
        lastFailureMessage_ = std::move(message);
    }

    void notifyConnect(bool success, const std::string& message) {
        if (pendingConnectCallback_) {
            auto callback = std::exchange(pendingConnectCallback_, nullptr);
            callback(success, message);
        }
        if (connectCallback_) {
            connectCallback_(success, message);
        }
    }

    void markConnected() {
        stopConnectTimer();
        connected_.store(true, std::memory_order_release);
        armReadTimer();
        notifyConnect(true, "");
    }

    void resetTransportState() {
        std::lock_guard<std::recursive_mutex> callbackLock(callbackMutex_);
        connected_.store(false, std::memory_order_release);
        running_.store(false, std::memory_order_release);
        localAddress_ = {};
        remoteAddress_ = {};
        clearLastFailure();
        tlsState_ = TlsState::Disabled;
        stopAllTimers();
        clearQueuesAndWaits();
        releaseTls();
        socket_.close();
    }

    void clearQueuesAndWaits() {
        std::lock_guard<std::mutex> lock(sendMutex_);
#ifdef _WIN32
        if (iocpWritePending_) {
            retainOutstandingWindowsIocpWriteBuffersLocked();
        } else {
            releaseRetainedWindowsIocpWriteBuffersLocked();
        }
#endif
        sendQueue_.clear();
        sendOffset_ = 0;
        pendingWriteBytes_ = 0;
        closeAfterFlush_ = false;
        flushRunning_ = false;
        flushReschedule_ = false;
        iocpWritePending_ = false;
#ifdef FASTNET_ENABLE_SSL
        tlsWriteQueue_.clear();
        tlsWriteOffset_ = 0;
#endif
        writeInterestEnabled_ = false;
        readWait_ = WaitDirection::None;
        writeWait_ = WaitDirection::None;
        handshakeWait_ = WaitDirection::None;
    }

    void cleanupInactiveTransport() {
        std::lock_guard<std::recursive_mutex> callbackLock(callbackMutex_);
        running_.store(false, std::memory_order_release);
        connected_.store(false, std::memory_order_release);
        stopAllTimers();
#ifdef _WIN32
        auto transport = std::move(windowsTransport_);
        if (transport) {
            transport->stop();
        } else {
            ioService_.getPoller().remove(socket_.getFd());
        }
#else
        ioService_.getPoller().remove(socket_.getFd());
#endif
        clearQueuesAndWaits();
        releaseTls();
        socket_.close();
    }

    void closeWithReason(const std::string& reason) {
        std::lock_guard<std::recursive_mutex> callbackLock(callbackMutex_);
        if (!running_.exchange(false, std::memory_order_acq_rel)) {
            stopAllTimers();
#ifdef _WIN32
            auto transport = std::move(windowsTransport_);
            if (transport) {
                transport->stop();
            }
#endif
            releaseTls();
            socket_.close();
            clearQueuesAndWaits();
            connected_.store(false, std::memory_order_release);
            return;
        }

#ifdef _WIN32
        auto transport = std::move(windowsTransport_);
        if (transport) {
            transport->stop();
        } else {
            ioService_.getPoller().remove(socket_.getFd());
        }
#else
        ioService_.getPoller().remove(socket_.getFd());
#endif

        const bool wasConnected = connected_.exchange(false, std::memory_order_acq_rel);
        stopAllTimers();
        clearQueuesAndWaits();
        releaseTls();
        socket_.close();

        if (wasConnected && disconnectCallback_) {
            disconnectCallback_(reason);
        }
    }

    void handleRead() {
        std::lock_guard<std::recursive_mutex> callbackLock(callbackMutex_);
        if (!running_.load(std::memory_order_acquire)) {
            return;
        }

        if (tlsState_ == TlsState::Handshaking) {
            if (!driveTlsHandshake()) {
                const Error lastError = getLastError();
                const std::string failureMessage =
                    lastError.isFailure() ? lastError.toString() : "TLS handshake failed";
                notifyConnect(false, failureMessage);
                handleTransportFailure(lastError.getCode(), failureMessage);
            }
            return;
        }

        constexpr size_t kReadChunkSize = 8192;
        while (running_.load(std::memory_order_acquire)) {
            std::shared_ptr<Buffer> sharedPayloadBuffer;
            Buffer directPayloadBuffer;
            void* readTarget = nullptr;
            size_t readCapacity = kReadChunkSize;
            std::array<std::uint8_t, kReadChunkSize> stackBuffer;
            const bool useDirectPayloadBuffer = ownedDataReceivedCallback_ || dataReceivedCallback_;
            if (sharedDataReceivedCallback_) {
                sharedPayloadBuffer = BufferPool::getInstance().allocateBuffer(kReadChunkSize);
                if (!sharedPayloadBuffer) {
                    handleTransportFailure(ErrorCode::UnknownError, "Receive buffer allocation failed");
                    return;
                }
                readTarget = sharedPayloadBuffer->data();
                readCapacity = sharedPayloadBuffer->size();
            } else if (useDirectPayloadBuffer) {
                directPayloadBuffer.resize(kReadChunkSize);
                readTarget = directPayloadBuffer.data();
                readCapacity = directPayloadBuffer.size();
            } else {
                readTarget = stackBuffer.data();
                readCapacity = stackBuffer.size();
            }

            const IoResult result = readTransport(readTarget, readCapacity);
            if (result.status == IoStatus::Ok) {
                setReadWait(WaitDirection::None);
                armReadTimer();
                if (sharedDataReceivedCallback_) {
                    sharedPayloadBuffer->resize(result.bytes);
                    std::shared_ptr<const Buffer> sharedPayload = sharedPayloadBuffer;
                    sharedDataReceivedCallback_(sharedPayload);
                } else if (useDirectPayloadBuffer) {
                    directPayloadBuffer.resize(result.bytes);
                    dispatchReceivedBuffer(std::move(directPayloadBuffer));
                } else {
                    processReceivedData(static_cast<const std::uint8_t*>(readTarget), result.bytes);
                }
                if (result.bytes < readCapacity) {
                    break;
                }
                continue;
            }

            if (result.status == IoStatus::WouldBlock) {
                setReadWait(result.wait);
                break;
            }

            if (result.status == IoStatus::Closed) {
                closeWithReason("Connection closed by peer");
                return;
            }

            handleTransportFailure(result.errorCode, result.message);
            return;
        }
    }

    void handleWrite() {
        std::lock_guard<std::recursive_mutex> callbackLock(callbackMutex_);
        if (!running_.load(std::memory_order_acquire)) {
            return;
        }

        if (tlsState_ == TlsState::Handshaking) {
            if (!driveTlsHandshake()) {
                const Error lastError = getLastError();
                const std::string failureMessage =
                    lastError.isFailure() ? lastError.toString() : "TLS handshake failed";
                notifyConnect(false, failureMessage);
                handleTransportFailure(lastError.getCode(), failureMessage);
            }
            return;
        }

        bool shouldScheduleFlush = false;
        {
            std::lock_guard<std::mutex> lock(sendMutex_);
            shouldScheduleFlush = markFlushScheduledLocked();
        }
        dispatchFlushIfNeeded(shouldScheduleFlush);
    }

    void dispatchFlushIfNeeded(bool shouldScheduleFlush) {
        if (!shouldScheduleFlush) {
            return;
        }
#ifdef _WIN32
        if (usingWindowsIocpTransport()) {
            startIocpWriteIfNeeded();
            return;
        }
#endif
        runQueuedFlush();
    }

    bool markFlushScheduledLocked() {
        if (!running_.load(std::memory_order_acquire)) {
            return false;
        }
        if (flushRunning_) {
            flushReschedule_ = true;
            return false;
        }
        flushRunning_ = true;
        flushReschedule_ = false;
        return true;
    }

    void runQueuedFlush() {
        while (connected_.load(std::memory_order_acquire)) {
            const bool drained = drainSendQueue();

            bool shouldContinue = false;
            {
                std::lock_guard<std::mutex> lock(sendMutex_);
                if (!running_.load(std::memory_order_acquire) || !drained) {
                    flushRunning_ = false;
                    flushReschedule_ = false;
                    return;
                }

                if (!flushReschedule_) {
                    flushRunning_ = false;
                    return;
                }

                flushReschedule_ = false;
                shouldContinue = true;
            }
            if (!shouldContinue) {
                return;
            }
        }

        std::lock_guard<std::mutex> lock(sendMutex_);
        flushRunning_ = false;
        flushReschedule_ = false;
    }

    bool drainSendQueue() {
        while (connected_.load(std::memory_order_acquire)) {
            QueuedWrite current;
            size_t offset = 0;
            bool shouldCloseAfterFlush = false;

            {
                std::lock_guard<std::mutex> lock(sendMutex_);
                if (sendQueue_.empty()) {
                    writeWait_ = WaitDirection::None;
#ifdef FASTNET_ENABLE_SSL
                    const bool hasTlsOutput = usingWindowsIocpTls() && hasPendingWindowsIocpTlsOutputLocked();
#else
                    const bool hasTlsOutput = false;
#endif
                    if (!hasTlsOutput) {
                        stopWriteTimer();
                        shouldCloseAfterFlush = closeAfterFlush_;
                        if (shouldCloseAfterFlush) {
                            closeAfterFlush_ = false;
                        }
                    } else {
                        armWriteTimer();
                    }
                    refreshInterestLocked();
                } else {
                    current = sendQueue_.front();
                    offset = sendOffset_;
                }
            }

            if (shouldCloseAfterFlush) {
                closeWithReason("Connection closed");
                return false;
            }
            if (current.empty()) {
                return true;
            }
            const std::uint8_t* payloadBytes = current.bytes();
            if (payloadBytes == nullptr) {
                return true;
            }
            const IoResult result = writeTransport(payloadBytes + offset, current.size() - offset);
            if (result.status == IoStatus::Ok) {
                setWriteWait(WaitDirection::None);

                bool closeNow = false;
                {
                    std::lock_guard<std::mutex> lock(sendMutex_);
                    sendOffset_ += result.bytes;
                    pendingWriteBytes_ = result.bytes >= pendingWriteBytes_
                                             ? 0
                                             : pendingWriteBytes_ - result.bytes;
                    if (sendOffset_ < current.size()) {
                        armWriteTimer();
                        refreshInterestLocked();
                        continue;
                    }

                    sendQueue_.pop_front();
                    sendOffset_ = 0;
                    if (sendQueue_.empty()) {
#ifdef FASTNET_ENABLE_SSL
                        const bool hasTlsOutput = usingWindowsIocpTls() && hasPendingWindowsIocpTlsOutputLocked();
#else
                        const bool hasTlsOutput = false;
#endif
                        if (!hasTlsOutput) {
                            stopWriteTimer();
                            closeNow = closeAfterFlush_;
                            if (closeNow) {
                                closeAfterFlush_ = false;
                            }
                        } else {
                            armWriteTimer();
                        }
                    } else {
                        armWriteTimer();
                    }
                    refreshInterestLocked();
                }
                if (closeNow) {
                    closeWithReason("Connection closed");
                    return false;
                }
                continue;
            }

            if (result.status == IoStatus::WouldBlock) {
                armWriteTimer();
                setWriteWait(result.wait);
                return false;
            }

            if (result.status == IoStatus::Closed) {
                closeWithReason("Connection closed by peer");
                return false;
            }

            handleTransportFailure(result.errorCode, result.message);
            return false;
        }
        return false;
    }

    void handleTransportFailure(ErrorCode code, const std::string& message) {
        setLastFailure(code, message);
        if (!connected_.load(std::memory_order_acquire) && pendingConnectCallback_) {
            notifyConnect(false, message);
        }
        if (errorCallback_) {
            errorCallback_(code, message);
        }
        closeWithReason(message);
    }

    void dispatchReceivedBuffer(Buffer&& payload) {
        if (payload.empty()) {
            return;
        }

        if (ownedDataReceivedCallback_) {
            ownedDataReceivedCallback_(std::move(payload));
            return;
        }

        if (dataReceivedCallback_) {
            dataReceivedCallback_(payload);
        }
    }

    void processReceivedData(const std::uint8_t* data, size_t size) {
        if (data == nullptr || size == 0) {
            return;
        }

        if (ownedDataReceivedCallback_ || dataReceivedCallback_) {
            Buffer payload(data, data + static_cast<std::ptrdiff_t>(size));
            dispatchReceivedBuffer(std::move(payload));
        }
    }

#ifdef _WIN32
    bool usingWindowsIocpTransport() const {
        return windowsTransport_ != nullptr;
    }

    bool shouldStartWindowsIocpFlushLocked() const {
#ifdef FASTNET_ENABLE_SSL
        if (usingWindowsIocpTls()) {
            return !iocpWritePending_ && tlsWriteQueue_.empty();
        }
#endif
        return !iocpWritePending_;
    }

#ifndef FASTNET_ENABLE_SSL
    Error submitNextWindowsIocpRawWriteLocked(std::shared_ptr<WindowsIocpSocketTransport>& transport,
                                              const std::uint8_t*& payload,
                                              size_t& payloadSize,
                                              bool& closeNow) {
        payload = nullptr;
        payloadSize = 0;
        closeNow = false;

        constexpr size_t kMaxIocpWriteSize = (std::numeric_limits<DWORD>::max)();
        if (!windowsTransport_ || iocpWritePending_) {
            return FASTNET_SUCCESS;
        }

        while (!sendQueue_.empty()) {
            const QueuedWrite& current = sendQueue_.front();
            const size_t currentSize = current.size();
            const std::uint8_t* currentBytes = current.bytes();
            if (sendOffset_ >= currentSize || currentBytes == nullptr) {
                sendQueue_.pop_front();
                sendOffset_ = 0;
                continue;
            }

            transport = windowsTransport_;
            payload = currentBytes + sendOffset_;
            payloadSize = (std::min)(currentSize - sendOffset_, kMaxIocpWriteSize);
            iocpWritePending_ = true;
            return FASTNET_SUCCESS;
        }

        stopWriteTimer();
        flushRunning_ = false;
        flushReschedule_ = false;
        if (closeAfterFlush_) {
            closeAfterFlush_ = false;
            closeNow = true;
        }
        return FASTNET_SUCCESS;
    }
#endif

#ifdef FASTNET_ENABLE_SSL
    bool usingWindowsIocpTls() const {
        return usingWindowsIocpTransport() && sslConfig_.enableSSL && sslHandle_ != nullptr;
    }

    bool hasPendingWindowsIocpTlsOutputLocked() const {
        return iocpWritePending_ || !tlsWriteQueue_.empty();
    }

    Error submitNextWindowsIocpRawWriteLocked(std::shared_ptr<WindowsIocpSocketTransport>& transport,
                                              const std::uint8_t*& payload,
                                              size_t& payloadSize,
                                              bool& closeNow) {
        payload = nullptr;
        payloadSize = 0;
        closeNow = false;

        constexpr size_t kMaxIocpWriteSize = (std::numeric_limits<DWORD>::max)();
        if (!windowsTransport_ || iocpWritePending_) {
            return FASTNET_SUCCESS;
        }

        if (usingWindowsIocpTls()) {
            if (!tlsWriteQueue_.empty()) {
                Buffer& encryptedChunk = tlsWriteQueue_.front();
                if (tlsWriteOffset_ >= encryptedChunk.size()) {
                    tlsWriteQueue_.pop_front();
                    tlsWriteOffset_ = 0;
                    return FASTNET_SUCCESS;
                }

                transport = windowsTransport_;
                payload = encryptedChunk.data() + tlsWriteOffset_;
                payloadSize = (std::min)(encryptedChunk.size() - tlsWriteOffset_, kMaxIocpWriteSize);
                iocpWritePending_ = true;
                return FASTNET_SUCCESS;
            }

            if (pendingWriteBytes_ == 0 && sendQueue_.empty()) {
                stopWriteTimer();
                flushRunning_ = false;
                flushReschedule_ = false;
                if (closeAfterFlush_) {
                    closeAfterFlush_ = false;
                    closeNow = true;
                }
            }
            return FASTNET_SUCCESS;
        }

        while (!sendQueue_.empty()) {
            const QueuedWrite& current = sendQueue_.front();
            const size_t currentSize = current.size();
            const std::uint8_t* currentBytes = current.bytes();
            if (sendOffset_ >= currentSize || currentBytes == nullptr) {
                sendQueue_.pop_front();
                sendOffset_ = 0;
                continue;
            }

            transport = windowsTransport_;
            payload = currentBytes + sendOffset_;
            payloadSize = (std::min)(currentSize - sendOffset_, kMaxIocpWriteSize);
            iocpWritePending_ = true;
            return FASTNET_SUCCESS;
        }

        stopWriteTimer();
        flushRunning_ = false;
        flushReschedule_ = false;
        if (closeAfterFlush_) {
            closeAfterFlush_ = false;
            closeNow = true;
        }
        return FASTNET_SUCCESS;
    }

    Error flushPendingWindowsIocpTlsCiphertext() {
        if (!usingWindowsIocpTls()) {
            return FASTNET_SUCCESS;
        }

        bool addedOutput = false;
        std::lock_guard<std::recursive_mutex> tlsLock(tlsMutex_);
        while (sslWriteBio_ != nullptr) {
            const int pending = BIO_pending(sslWriteBio_);
            if (pending <= 0) {
                break;
            }

            Buffer encryptedChunk(static_cast<size_t>(pending));
            const int bytesRead =
                BIO_read(sslWriteBio_, encryptedChunk.data(), static_cast<int>(encryptedChunk.size()));
            if (bytesRead <= 0) {
                break;
            }

            encryptedChunk.resize(static_cast<size_t>(bytesRead));
            std::lock_guard<std::mutex> lock(sendMutex_);
            tlsWriteQueue_.push_back(std::move(encryptedChunk));
            addedOutput = true;
        }
        if (addedOutput) {
            startIocpWriteIfNeeded();
        }
        return FASTNET_SUCCESS;
    }

    bool pumpWindowsIocpTlsPlaintext() {
        constexpr size_t kReadChunkSize = 8192;

        while (running_.load(std::memory_order_acquire) && tlsState_ == TlsState::Ready) {
            std::shared_ptr<Buffer> sharedPayloadBuffer;
            Buffer directPayloadBuffer;
            std::array<std::uint8_t, kReadChunkSize> stackBuffer{};
            std::uint8_t* readTarget = nullptr;
            size_t readCapacity = kReadChunkSize;
            const bool useDirectPayloadBuffer = ownedDataReceivedCallback_ || dataReceivedCallback_;
            if (sharedDataReceivedCallback_) {
                sharedPayloadBuffer = BufferPool::getInstance().allocateBuffer(kReadChunkSize);
                if (!sharedPayloadBuffer) {
                    handleTransportFailure(ErrorCode::UnknownError, "Receive buffer allocation failed");
                    return false;
                }
                readTarget = sharedPayloadBuffer->data();
                readCapacity = sharedPayloadBuffer->size();
            } else if (useDirectPayloadBuffer) {
                directPayloadBuffer.resize(kReadChunkSize);
                readTarget = directPayloadBuffer.data();
                readCapacity = directPayloadBuffer.size();
            } else {
                readTarget = stackBuffer.data();
                readCapacity = stackBuffer.size();
            }

            int received = 0;
            int sslError = SSL_ERROR_NONE;
            {
                std::lock_guard<std::recursive_mutex> tlsLock(tlsMutex_);
                if (!running_.load(std::memory_order_acquire) || tlsState_ != TlsState::Ready || sslHandle_ == nullptr) {
                    return true;
                }
                ERR_clear_error();
                received = SSL_read(sslHandle_, readTarget, static_cast<int>(readCapacity));
                if (received <= 0) {
                    sslError = SSL_get_error(sslHandle_, received);
                }
            }

            if (received > 0) {
                setReadWait(WaitDirection::None);
                armReadTimer();
                if (sharedDataReceivedCallback_) {
                    sharedPayloadBuffer->resize(static_cast<size_t>(received));
                    std::shared_ptr<const Buffer> sharedPayload = sharedPayloadBuffer;
                    sharedDataReceivedCallback_(sharedPayload);
                } else if (useDirectPayloadBuffer) {
                    directPayloadBuffer.resize(static_cast<size_t>(received));
                    dispatchReceivedBuffer(std::move(directPayloadBuffer));
                } else {
                    processReceivedData(stackBuffer.data(), static_cast<size_t>(received));
                }
                continue;
            }

            switch (sslError) {
                case SSL_ERROR_WANT_READ:
                    setReadWait(WaitDirection::Read);
                    return true;
                case SSL_ERROR_WANT_WRITE:
                    setReadWait(WaitDirection::Write);
                    if (flushPendingWindowsIocpTlsCiphertext().isFailure()) {
                        setLastFailure(ErrorCode::SSLError, "Failed to flush pending TLS ciphertext");
                        return false;
                    }
                    return true;
                case SSL_ERROR_ZERO_RETURN:
                    closeWithReason("Connection closed by peer");
                    return false;
                default:
                    setLastFailure(ErrorCode::SSLError, formatOpenSslFailure("TLS read failed"));
                    return false;
            }
        }

        return true;
    }
#endif

    void startIocpWriteIfNeeded() {
        std::shared_ptr<WindowsIocpSocketTransport> transport;
        const std::uint8_t* payload = nullptr;
        size_t payloadSize = 0;
        bool closeNow = false;
        Error prepareError = FASTNET_SUCCESS;

        {
            std::lock_guard<std::mutex> lock(sendMutex_);
            if (!running_.load(std::memory_order_acquire) || !windowsTransport_) {
                return;
            }
            prepareError = submitNextWindowsIocpRawWriteLocked(transport, payload, payloadSize, closeNow);
        }

        if (prepareError.isFailure()) {
            handleTransportFailure(prepareError.getCode(), prepareError.toString());
            return;
        }

        if (closeNow) {
            closeWithReason("Connection closed");
            return;
        }

#ifdef FASTNET_ENABLE_SSL
        if (usingWindowsIocpTls() && payload == nullptr && payloadSize == 0) {
            if (tlsState_ == TlsState::Ready && connected_.load(std::memory_order_acquire)) {
                runQueuedFlush();
            }
            return;
        }
#endif
        if (!transport || payload == nullptr || payloadSize == 0) {
            return;
        }

        const Error submitError = transport->submitWrite(payload, payloadSize);
        if (submitError.isFailure()) {
            {
                std::lock_guard<std::mutex> lock(sendMutex_);
                iocpWritePending_ = false;
                flushRunning_ = false;
                flushReschedule_ = false;
            }
            handleTransportFailure(submitError.getCode(), submitError.toString());
        }
    }

    void handleWindowsIocpRead(const std::uint8_t* data, size_t size) override {
        std::lock_guard<std::recursive_mutex> callbackLock(callbackMutex_);
        if (!running_.load(std::memory_order_acquire)) {
            return;
        }
#ifdef FASTNET_ENABLE_SSL
        if (usingWindowsIocpTls()) {
            {
                std::lock_guard<std::recursive_mutex> tlsLock(tlsMutex_);
                if (sslReadBio_ == nullptr ||
                    BIO_write(sslReadBio_, data, static_cast<int>(size)) != static_cast<int>(size)) {
                    handleTransportFailure(ErrorCode::SSLError, "Failed to feed TLS ciphertext into read BIO");
                    return;
                }
            }

            if (tlsState_ == TlsState::Handshaking) {
                if (!driveTlsHandshake()) {
                    const Error lastError = getLastError();
                    const std::string failureMessage =
                        lastError.isFailure() ? lastError.toString() : "TLS handshake failed";
                    notifyConnect(false, failureMessage);
                    handleTransportFailure(lastError.getCode(), failureMessage);
                    return;
                }

                if (tlsState_ != TlsState::Ready) {
                    return;
                }

                if (!pumpWindowsIocpTlsPlaintext()) {
                    const Error lastError = getLastError();
                    handleTransportFailure(lastError.getCode(), lastError.isFailure() ? lastError.toString()
                                                                                      : "TLS read failed");
                }
            }

            if (!pumpWindowsIocpTlsPlaintext()) {
                const Error lastError = getLastError();
                handleTransportFailure(lastError.getCode(), lastError.isFailure() ? lastError.toString()
                                                                                  : "TLS read failed");
                return;
            }

            bool shouldResumeTlsWrite = false;
            {
                std::lock_guard<std::mutex> lock(sendMutex_);
                shouldResumeTlsWrite = writeWait_ == WaitDirection::Read;
            }
            if (shouldResumeTlsWrite && connected_.load(std::memory_order_acquire)) {
                runQueuedFlush();
            }
            return;
        }
#endif
        armReadTimer();
        if (sharedDataReceivedCallback_) {
            auto sharedPayloadBuffer = BufferPool::getInstance().allocateBuffer(size);
            if (!sharedPayloadBuffer) {
                handleTransportFailure(ErrorCode::UnknownError, "Receive buffer allocation failed");
                return;
            }
            if (size != 0) {
                std::memcpy(sharedPayloadBuffer->data(), data, size);
                sharedPayloadBuffer->resize(size);
            }
            std::shared_ptr<const Buffer> sharedPayload = sharedPayloadBuffer;
            sharedDataReceivedCallback_(sharedPayload);
            return;
        }
        processReceivedData(data, size);
    }

    void handleWindowsIocpReadClosed() override {
        std::lock_guard<std::recursive_mutex> callbackLock(callbackMutex_);
        if (running_.load(std::memory_order_acquire)) {
            closeWithReason("Connection closed by peer");
        }
    }

    void handleWindowsIocpReadError(int systemError) override {
        std::lock_guard<std::recursive_mutex> callbackLock(callbackMutex_);
        if (!running_.load(std::memory_order_acquire)) {
            return;
        }
        handleTransportFailure(ErrorCode::SocketError, SocketWrapper::getErrorMessage(systemError));
    }

    void handleWindowsIocpWriteCompleted(size_t bytesTransferred) override {
        std::lock_guard<std::recursive_mutex> callbackLock(callbackMutex_);
        if (!running_.load(std::memory_order_acquire)) {
            return;
        }

        bool closeNow = false;
        {
            std::lock_guard<std::mutex> lock(sendMutex_);
            iocpWritePending_ = false;
#ifdef FASTNET_ENABLE_SSL
            if (usingWindowsIocpTls()) {
                if (!tlsWriteQueue_.empty()) {
                    tlsWriteOffset_ += bytesTransferred;
                    while (!tlsWriteQueue_.empty()) {
                        const Buffer& current = tlsWriteQueue_.front();
                        if (tlsWriteOffset_ < current.size()) {
                            break;
                        }
                        tlsWriteQueue_.pop_front();
                        tlsWriteOffset_ = 0;
                    }
                }

                const bool hasApplicationData = pendingWriteBytes_ != 0 || !sendQueue_.empty();
                const bool hasEncryptedData = !tlsWriteQueue_.empty();
                if (!hasApplicationData && !hasEncryptedData) {
                    stopWriteTimer();
                    flushRunning_ = false;
                    flushReschedule_ = false;
                    closeNow = closeAfterFlush_;
                    if (closeNow) {
                        closeAfterFlush_ = false;
                    }
                } else {
                    armWriteTimer();
                }
            } else
#endif
            {
                if (!sendQueue_.empty()) {
                    sendOffset_ += bytesTransferred;
                    pendingWriteBytes_ = bytesTransferred >= pendingWriteBytes_
                                             ? 0
                                             : pendingWriteBytes_ - bytesTransferred;
                    while (!sendQueue_.empty()) {
                        const size_t currentSize = sendQueue_.front().size();
                        if (sendOffset_ < currentSize) {
                            break;
                        }
                        sendQueue_.pop_front();
                        sendOffset_ = 0;
                    }
                }

                if (sendQueue_.empty()) {
                    stopWriteTimer();
                    flushRunning_ = false;
                    flushReschedule_ = false;
                    closeNow = closeAfterFlush_;
                    if (closeNow) {
                        closeAfterFlush_ = false;
                    }
                } else {
                    armWriteTimer();
                }
            }
#ifdef _WIN32
            releaseRetainedWindowsIocpWriteBuffersLocked();
#endif
        }

#ifdef FASTNET_ENABLE_SSL
        if (usingWindowsIocpTls()) {
            if (tlsState_ == TlsState::Handshaking) {
                if (!driveTlsHandshake()) {
                    const Error lastError = getLastError();
                    const std::string failureMessage =
                        lastError.isFailure() ? lastError.toString() : "TLS handshake failed";
                    notifyConnect(false, failureMessage);
                    handleTransportFailure(lastError.getCode(), failureMessage);
                    return;
                }
            }

            bool shouldResumeTlsRead = false;
            {
                std::lock_guard<std::mutex> lock(sendMutex_);
                shouldResumeTlsRead = readWait_ == WaitDirection::Write;
            }
            if (tlsState_ == TlsState::Ready && shouldResumeTlsRead) {
                if (!pumpWindowsIocpTlsPlaintext()) {
                    const Error lastError = getLastError();
                    handleTransportFailure(lastError.getCode(), lastError.isFailure() ? lastError.toString()
                                                                                      : "TLS read failed");
                    return;
                }
            }
        }
#endif

        if (closeNow) {
            closeWithReason("Connection closed");
            return;
        }
        startIocpWriteIfNeeded();
    }

    void handleWindowsIocpWriteError(int systemError) override {
        std::lock_guard<std::recursive_mutex> callbackLock(callbackMutex_);
        {
            std::lock_guard<std::mutex> lock(sendMutex_);
            iocpWritePending_ = false;
            flushRunning_ = false;
            flushReschedule_ = false;
#ifdef _WIN32
            releaseRetainedWindowsIocpWriteBuffersLocked();
#endif
        }
        if (!running_.load(std::memory_order_acquire)) {
            return;
        }
        handleTransportFailure(ErrorCode::SocketError, SocketWrapper::getErrorMessage(systemError));
    }
#endif

    void armConnectTimer() {
        if (!connectTimer_ || connectTimeout_ == 0) {
            return;
        }

        std::weak_ptr<Impl> weakSelf = shared_from_this();
        connectTimer_->start(std::chrono::milliseconds(connectTimeout_), [weakSelf]() {
            auto self = weakSelf.lock();
            if (!self ||
                !self->running_.load(std::memory_order_acquire) ||
                self->connected_.load(std::memory_order_acquire)) {
                return;
            }
            self->handleTransportFailure(ErrorCode::TimeoutError, "Connection timed out");
        });
    }

    void stopConnectTimer() {
        if (connectTimer_) {
            connectTimer_->stop();
        }
    }

    void armReadTimer() {
        if (!readTimer_ || readTimeout_ == 0 || !connected_.load(std::memory_order_acquire)) {
            return;
        }

        std::weak_ptr<Impl> weakSelf = shared_from_this();
        readTimer_->start(std::chrono::milliseconds(readTimeout_), [weakSelf]() {
            auto self = weakSelf.lock();
            if (!self || !self->connected_.load(std::memory_order_acquire)) {
                return;
            }
            self->handleTransportFailure(ErrorCode::TimeoutError, "Read timed out");
        });
    }

    void stopReadTimer() {
        if (readTimer_) {
            readTimer_->stop();
        }
    }

    void armWriteTimer() {
        if (!writeTimer_ || writeTimeout_ == 0 || !connected_.load(std::memory_order_acquire)) {
            return;
        }

        std::weak_ptr<Impl> weakSelf = shared_from_this();
        writeTimer_->start(std::chrono::milliseconds(writeTimeout_), [weakSelf]() {
            auto self = weakSelf.lock();
            if (!self || !self->connected_.load(std::memory_order_acquire)) {
                return;
            }
            bool hasPendingWrite = false;
            {
                std::lock_guard<std::mutex> lock(self->sendMutex_);
                hasPendingWrite = self->pendingWriteBytes_ != 0;
            }
            if (!hasPendingWrite) {
                return;
            }
            self->handleTransportFailure(ErrorCode::TimeoutError, "Write timed out");
        });
    }

    void stopWriteTimer() {
        if (writeTimer_) {
            writeTimer_->stop();
        }
    }

    void stopAllTimers() {
        stopConnectTimer();
        stopReadTimer();
        stopWriteTimer();
    }

    void updateAddresses(const std::string& host, uint16_t port) {
        std::string localIp;
        uint16_t localPort = 0;
        if (socket_.getLocalAddress(localIp, localPort).isSuccess()) {
            localAddress_ = Address(localIp, localPort);
        }

        std::string remoteIp;
        uint16_t remotePort = 0;
        if (socket_.getRemoteAddress(remoteIp, remotePort).isSuccess()) {
            remoteAddress_ = Address(remoteIp, remotePort);
        } else {
            remoteAddress_ = Address(host, port);
        }
    }

    bool initializeTls() {
#ifndef FASTNET_ENABLE_SSL
        setLastFailure(ErrorCode::SSLError, "SSL requested but FastNet was built without FASTNET_ENABLE_SSL");
        return false;
#else
        std::lock_guard<std::recursive_mutex> tlsLock(tlsMutex_);
        if (!sslContext_.initialize(sslConfig_, SSLContext::Mode::Client)) {
            const Error sslError = sslContext_.getLastError();
            setLastFailure(sslError.getCode(), sslError.toString());
            return false;
        }

        sslHandle_ = sslContext_.createHandle();
        if (sslHandle_ == nullptr) {
            const Error sslError = sslContext_.getLastError();
            if (sslError.isFailure()) {
                setLastFailure(sslError.getCode(), sslError.toString());
            } else {
                setLastFailure(ErrorCode::SSLError, formatOpenSslFailure("Failed to create SSL handle"));
            }
            sslContext_.cleanup();
            return false;
        }

#ifdef _WIN32
        if (usingWindowsIocpTransport()) {
            sslReadBio_ = BIO_new(BIO_s_mem());
            sslWriteBio_ = BIO_new(BIO_s_mem());
            if (sslReadBio_ == nullptr || sslWriteBio_ == nullptr) {
                setLastFailure(ErrorCode::SSLError, formatOpenSslFailure("Failed to create TLS memory BIOs"));
                releaseTls();
                return false;
            }
            SSL_set_bio(sslHandle_, sslReadBio_, sslWriteBio_);
        } else
#endif
        {
            if (SSL_set_fd(sslHandle_, static_cast<int>(socket_.getFd())) != 1) {
                setLastFailure(ErrorCode::SSLError, formatOpenSslFailure("Failed to bind SSL handle to socket"));
                releaseTls();
                return false;
            }
        }

        tlsState_ = TlsState::Handshaking;
        handshakeWait_ = WaitDirection::None;
        return true;
#endif
    }

    bool driveTlsHandshake() {
#ifndef FASTNET_ENABLE_SSL
        setLastFailure(ErrorCode::SSLError, "SSL requested but FastNet was built without FASTNET_ENABLE_SSL");
        return false;
#else
        bool handshakeCompleted = false;
        bool shouldFlushPendingCiphertext = false;
        {
            std::lock_guard<std::recursive_mutex> tlsLock(tlsMutex_);
            if (tlsState_ != TlsState::Handshaking || sslHandle_ == nullptr) {
                return true;
            }

            ERR_clear_error();
            const int result = SSL_do_handshake(sslHandle_);
            if (result == 1) {
                if (sslConfig_.verifyPeer) {
                    const long verifyResult = SSL_get_verify_result(sslHandle_);
                    if (verifyResult != X509_V_OK) {
                        setLastFailure(ErrorCode::SSLCertificateError, formatVerifyFailure(verifyResult));
                        return false;
                    }
                }
                tlsState_ = TlsState::Ready;
                handshakeCompleted = true;
                shouldFlushPendingCiphertext = true;
            } else {
                const int sslError = SSL_get_error(sslHandle_, result);
                switch (sslError) {
                    case SSL_ERROR_WANT_READ:
                        setHandshakeWait(WaitDirection::Read);
                        shouldFlushPendingCiphertext = true;
                        break;
                    case SSL_ERROR_WANT_WRITE:
                        setHandshakeWait(WaitDirection::Write);
                        shouldFlushPendingCiphertext = true;
                        break;
                    case SSL_ERROR_ZERO_RETURN:
                        setLastFailure(ErrorCode::SSLHandshakeError, "TLS handshake aborted by peer");
                        return false;
                    default:
                        setLastFailure(ErrorCode::SSLHandshakeError, formatOpenSslFailure("TLS handshake failed"));
                        return false;
                }
            }
        }

#ifdef _WIN32
        if (usingWindowsIocpTransport() && shouldFlushPendingCiphertext) {
            const Error flushError = flushPendingWindowsIocpTlsCiphertext();
            if (flushError.isFailure()) {
                setLastFailure(flushError.getCode(), flushError.toString());
                return false;
            }
        }
#endif
        if (handshakeCompleted) {
            setHandshakeWait(WaitDirection::None);
            markConnected();
        }
        return true;
#endif
    }

    IoResult readTransport(void* buffer, size_t len) {
        if (tlsState_ == TlsState::Ready) {
#ifndef FASTNET_ENABLE_SSL
            return {IoStatus::Error, 0, WaitDirection::None, ErrorCode::SSLError,
                    "SSL requested but FastNet was built without FASTNET_ENABLE_SSL"};
#else
            ERR_clear_error();
            const int received = SSL_read(sslHandle_, buffer, static_cast<int>(len));
            if (received > 0) {
                return {IoStatus::Ok, static_cast<size_t>(received)};
            }

            const int sslError = SSL_get_error(sslHandle_, received);
            switch (sslError) {
                case SSL_ERROR_WANT_READ:
                    return {IoStatus::WouldBlock, 0, WaitDirection::Read};
                case SSL_ERROR_WANT_WRITE:
                    return {IoStatus::WouldBlock, 0, WaitDirection::Write};
                case SSL_ERROR_ZERO_RETURN:
                    return {IoStatus::Closed};
                default:
                    return {IoStatus::Error, 0, WaitDirection::None, ErrorCode::SSLError,
                            formatOpenSslFailure("TLS read failed")};
            }
#endif
        }

        const int received = socket_.recv(buffer, len);
        if (received > 0) {
            return {IoStatus::Ok, static_cast<size_t>(received)};
        }
        if (received == 0) {
            return {IoStatus::Closed};
        }

        const int error = SocketWrapper::getLastSocketError();
        if (isSocketWouldBlock(error)) {
            return {IoStatus::WouldBlock, 0, WaitDirection::Read};
        }
        return {IoStatus::Error, 0, WaitDirection::None, ErrorCode::SocketError,
                SocketWrapper::getErrorMessage(error)};
    }

    IoResult writeTransport(const void* data, size_t len) {
        if (tlsState_ == TlsState::Ready) {
#ifndef FASTNET_ENABLE_SSL
            return {IoStatus::Error, 0, WaitDirection::None, ErrorCode::SSLError,
                    "SSL requested but FastNet was built without FASTNET_ENABLE_SSL"};
#else
            std::lock_guard<std::recursive_mutex> tlsLock(tlsMutex_);
#ifdef _WIN32
            if (usingWindowsIocpTransport()) {
                {
                    std::lock_guard<std::mutex> lock(sendMutex_);
                    if (hasPendingWindowsIocpTlsOutputLocked()) {
                        return {IoStatus::WouldBlock, 0, WaitDirection::Write};
                    }
                }

                ERR_clear_error();
                const int sent = SSL_write(sslHandle_, data, static_cast<int>(len));
                const Error flushError = flushPendingWindowsIocpTlsCiphertext();
                if (flushError.isFailure()) {
                    return {IoStatus::Error, 0, WaitDirection::None, flushError.getCode(), flushError.toString()};
                }

                if (sent > 0) {
                    return {IoStatus::Ok, static_cast<size_t>(sent)};
                }

                const int sslError = SSL_get_error(sslHandle_, sent);
                switch (sslError) {
                    case SSL_ERROR_WANT_READ:
                        return {IoStatus::WouldBlock, 0, WaitDirection::Read};
                    case SSL_ERROR_WANT_WRITE:
                        return {IoStatus::WouldBlock, 0, WaitDirection::Write};
                    case SSL_ERROR_ZERO_RETURN:
                        return {IoStatus::Closed};
                    default:
                        return {IoStatus::Error, 0, WaitDirection::None, ErrorCode::SSLError,
                                formatOpenSslFailure("TLS write failed")};
                }
            }
#endif
            ERR_clear_error();
            const int sent = SSL_write(sslHandle_, data, static_cast<int>(len));
            if (sent > 0) {
                return {IoStatus::Ok, static_cast<size_t>(sent)};
            }

            const int sslError = SSL_get_error(sslHandle_, sent);
            switch (sslError) {
                case SSL_ERROR_WANT_READ:
                    return {IoStatus::WouldBlock, 0, WaitDirection::Read};
                case SSL_ERROR_WANT_WRITE:
                    return {IoStatus::WouldBlock, 0, WaitDirection::Write};
                case SSL_ERROR_ZERO_RETURN:
                    return {IoStatus::Closed};
                default:
                    return {IoStatus::Error, 0, WaitDirection::None, ErrorCode::SSLError,
                            formatOpenSslFailure("TLS write failed")};
            }
#endif
        }

        const int sent = socket_.send(data, len);
        if (sent > 0) {
            return {IoStatus::Ok, static_cast<size_t>(sent)};
        }
        if (sent == 0) {
            return {IoStatus::Closed};
        }

        const int error = SocketWrapper::getLastSocketError();
        if (isSocketWouldBlock(error)) {
            return {IoStatus::WouldBlock, 0, WaitDirection::Write};
        }
        return {IoStatus::Error, 0, WaitDirection::None, ErrorCode::SocketError,
                SocketWrapper::getErrorMessage(error)};
    }

    void setReadWait(WaitDirection wait) {
        std::lock_guard<std::mutex> lock(sendMutex_);
#ifdef _WIN32
        if (usingWindowsIocpTransport()) {
            readWait_ = wait;
            return;
        }
#endif
        readWait_ = wait;
        refreshInterestLocked();
    }

    void setWriteWait(WaitDirection wait) {
        std::lock_guard<std::mutex> lock(sendMutex_);
#ifdef _WIN32
        if (usingWindowsIocpTransport()) {
            writeWait_ = wait;
            return;
        }
#endif
        writeWait_ = wait;
        refreshInterestLocked();
    }

    void setHandshakeWait(WaitDirection wait) {
        std::lock_guard<std::mutex> lock(sendMutex_);
#ifdef _WIN32
        if (usingWindowsIocpTransport()) {
            handshakeWait_ = wait;
            return;
        }
#endif
        handshakeWait_ = wait;
        refreshInterestLocked();
    }

    void refreshInterestLocked() {
        if (!running_.load(std::memory_order_acquire)) {
            writeInterestEnabled_ = false;
            return;
        }
#ifdef _WIN32
        if (usingWindowsIocpTransport()) {
            writeInterestEnabled_ = false;
            return;
        }
#endif

        bool shouldEnableWrite = false;
        if (tlsState_ == TlsState::Handshaking) {
            shouldEnableWrite = handshakeWait_ == WaitDirection::Write;
        } else {
            const bool hasPendingOutput = !sendQueue_.empty();
            if (hasPendingOutput) {
                shouldEnableWrite = writeWait_ != WaitDirection::Read;
            }
            if (!shouldEnableWrite) {
                shouldEnableWrite = readWait_ == WaitDirection::Write || writeWait_ == WaitDirection::Write;
            }
        }

        if (writeInterestEnabled_ == shouldEnableWrite) {
            return;
        }

        EventType events = EventType::Read | EventType::Error | EventType::Close;
        if (shouldEnableWrite) {
            events |= EventType::Write;
        }
        ioService_.getPoller().modify(socket_.getFd(), events);
        writeInterestEnabled_ = shouldEnableWrite;
    }

    void releaseTls() {
#ifdef FASTNET_ENABLE_SSL
        std::lock_guard<std::recursive_mutex> tlsLock(tlsMutex_);
        if (sslHandle_ != nullptr) {
            ERR_clear_error();
            SSL_shutdown(sslHandle_);
            SSL_free(sslHandle_);
            sslHandle_ = nullptr;
        }
        sslReadBio_ = nullptr;
        sslWriteBio_ = nullptr;
        tlsWriteQueue_.clear();
        tlsWriteOffset_ = 0;
#endif
        sslContext_.cleanup();
        tlsState_ = TlsState::Disabled;
    }

    IoService& ioService_;
    SocketWrapper socket_;
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    mutable std::mutex failureMutex_;
    mutable std::recursive_mutex callbackMutex_;
    Address localAddress_;
    Address remoteAddress_;
    uint32_t connectTimeout_ = 5000;
    uint32_t readTimeout_ = 0;
    uint32_t writeTimeout_ = 0;

    ConnectCallback connectCallback_;
    ConnectCallback pendingConnectCallback_;
    DisconnectCallback disconnectCallback_;
    DataReceivedCallback dataReceivedCallback_;
    OwnedDataReceivedCallback ownedDataReceivedCallback_;
    SharedDataReceivedCallback sharedDataReceivedCallback_;
    ErrorCallback errorCallback_;

    mutable std::mutex sendMutex_;
    std::deque<QueuedWrite> sendQueue_;
    size_t sendOffset_ = 0;
    size_t pendingWriteBytes_ = 0;
    bool closeAfterFlush_ = false;
    bool flushRunning_ = false;
    bool flushReschedule_ = false;
    bool iocpWritePending_ = false;
    bool writeInterestEnabled_ = false;
    WaitDirection readWait_ = WaitDirection::None;
    WaitDirection writeWait_ = WaitDirection::None;
    WaitDirection handshakeWait_ = WaitDirection::None;
    TlsState tlsState_ = TlsState::Disabled;
    SSLConfig sslConfig_;
    SSLContext sslContext_;
    ErrorCode lastFailureCode_ = ErrorCode::Success;
    std::string lastFailureMessage_;
    std::unique_ptr<Timer> connectTimer_;
    std::unique_ptr<Timer> readTimer_;
    std::unique_ptr<Timer> writeTimer_;

#ifdef FASTNET_ENABLE_SSL
    mutable std::recursive_mutex tlsMutex_;
    SSL* sslHandle_ = nullptr;
    BIO* sslReadBio_ = nullptr;
    BIO* sslWriteBio_ = nullptr;
    std::deque<Buffer> tlsWriteQueue_;
    size_t tlsWriteOffset_ = 0;
#endif
#ifdef _WIN32
    std::shared_ptr<WindowsIocpSocketTransport> windowsTransport_;
    std::vector<std::unique_ptr<std::deque<QueuedWrite>>> retainedSendBatches_;
#ifdef FASTNET_ENABLE_SSL
    std::vector<std::unique_ptr<std::deque<Buffer>>> retainedTlsWriteBatches_;
#endif
#endif
};

TcpClient::TcpClient(IoService& ioService)
    : impl_(std::make_shared<Impl>(ioService)) {}

TcpClient::~TcpClient() = default;

bool TcpClient::connect(const std::string& host,
                        uint16_t port,
                        const ConnectCallback& callback,
                        const SSLConfig& sslConfig) {
    return impl_->connect(host, port, callback, sslConfig);
}

bool TcpClient::connect(const Address& remoteAddress,
                        const ConnectCallback& callback,
                        const SSLConfig& sslConfig) {
    return impl_->connect(remoteAddress.host(), remoteAddress.port, callback, sslConfig);
}

void TcpClient::disconnect() {
    impl_->disconnect();
}

void TcpClient::disconnectAfterPendingWrites() {
    impl_->disconnectAfterPendingWrites();
}

bool TcpClient::send(const Buffer& data) {
    return impl_->send(data);
}

bool TcpClient::send(Buffer&& data) {
    return impl_->send(std::move(data));
}

bool TcpClient::send(const std::shared_ptr<const Buffer>& data) {
    return impl_->send(data);
}

bool TcpClient::send(std::string&& data) {
    return impl_->send(std::move(data));
}

bool TcpClient::send(std::string_view data) {
    return impl_->send(data);
}

bool TcpClient::send(const std::shared_ptr<const std::string>& data) {
    return impl_->send(data);
}

void TcpClient::setConnectCallback(const ConnectCallback& callback) {
    connectCallback_ = callback;
    impl_->setCallbacks(
        connectCallback_,
        disconnectCallback_,
        dataReceivedCallback_,
        ownedDataReceivedCallback_,
        sharedDataReceivedCallback_,
        errorCallback_);
}

void TcpClient::setDisconnectCallback(const DisconnectCallback& callback) {
    disconnectCallback_ = callback;
    impl_->setCallbacks(
        connectCallback_,
        disconnectCallback_,
        dataReceivedCallback_,
        ownedDataReceivedCallback_,
        sharedDataReceivedCallback_,
        errorCallback_);
}

void TcpClient::setDataReceivedCallback(const DataReceivedCallback& callback) {
    dataReceivedCallback_ = callback;
    ownedDataReceivedCallback_ = nullptr;
    sharedDataReceivedCallback_ = nullptr;
    impl_->setCallbacks(
        connectCallback_,
        disconnectCallback_,
        dataReceivedCallback_,
        ownedDataReceivedCallback_,
        sharedDataReceivedCallback_,
        errorCallback_);
}

void TcpClient::setOwnedDataReceivedCallback(const OwnedDataReceivedCallback& callback) {
    ownedDataReceivedCallback_ = callback;
    dataReceivedCallback_ = nullptr;
    sharedDataReceivedCallback_ = nullptr;
    impl_->setCallbacks(
        connectCallback_,
        disconnectCallback_,
        dataReceivedCallback_,
        ownedDataReceivedCallback_,
        sharedDataReceivedCallback_,
        errorCallback_);
}

void TcpClient::setSharedDataReceivedCallback(const SharedDataReceivedCallback& callback) {
    sharedDataReceivedCallback_ = callback;
    dataReceivedCallback_ = nullptr;
    ownedDataReceivedCallback_ = nullptr;
    impl_->setCallbacks(
        connectCallback_,
        disconnectCallback_,
        dataReceivedCallback_,
        ownedDataReceivedCallback_,
        sharedDataReceivedCallback_,
        errorCallback_);
}

void TcpClient::setErrorCallback(const ErrorCallback& callback) {
    errorCallback_ = callback;
    impl_->setCallbacks(
        connectCallback_,
        disconnectCallback_,
        dataReceivedCallback_,
        ownedDataReceivedCallback_,
        sharedDataReceivedCallback_,
        errorCallback_);
}

Address TcpClient::getLocalAddress() const {
    return impl_->getLocalAddress();
}

Address TcpClient::getRemoteAddress() const {
    return impl_->getRemoteAddress();
}

bool TcpClient::isConnected() const {
    return impl_->isConnected();
}

bool TcpClient::isSecure() const {
    return impl_->isSecure();
}

size_t TcpClient::getPendingWriteBytes() const {
    return impl_->getPendingWriteBytes();
}

Error TcpClient::getLastError() const {
    return impl_->getLastError();
}

void TcpClient::setConnectTimeout(uint32_t timeoutMs) {
    impl_->setConnectTimeout(timeoutMs);
}

void TcpClient::setReadTimeout(uint32_t timeoutMs) {
    impl_->setReadTimeout(timeoutMs);
}

void TcpClient::setWriteTimeout(uint32_t timeoutMs) {
    impl_->setWriteTimeout(timeoutMs);
}

} // namespace FastNet
