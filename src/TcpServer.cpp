/**
 * @file TcpServer.cpp
 * @brief FastNet TCP server implementation
 */
#include "TcpServer.h"

#include "Error.h"
#include "MemoryPool.h"
#include "SSLContext.h"
#include "SocketWrapper.h"
#include "Timer.h"
#include "SharedSessionTypes.h"
#include "WindowsIocpTransport.h"

#include <algorithm>
#include <unordered_map>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <exception>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef FASTNET_ENABLE_SSL
#include <openssl/err.h>
#include <openssl/x509_vfy.h>
#endif

#ifdef _WIN32
  #include <fileapi.h>
  #include <handleapi.h>
  #include <ioapiset.h>
  #include <mswsock.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/uio.h>  // writev scatter-gather
#include <unistd.h>
#endif

namespace FastNet {

namespace {



struct SessionTimeoutConfig {
    uint32_t connectionTimeoutMs = 0;
    uint32_t readTimeoutMs = 0;
    uint32_t writeTimeoutMs = 0;
};

constexpr size_t kBufferedFileChunkSize   = 64 * 1024;
constexpr size_t kDirectFileSendChunkSize = 1024 * 1024;
// Maximum number of iovec/WSABUF entries for scatter-gather sends.
// Chosen to fit in a stack array; virtually no real message will exceed this.
constexpr size_t kMaxScatterGatherIov = 64;

#ifndef _WIN32
/**
 * Attempt a single writev() across up to kMaxScatterGatherIov pending
 * Buffer-type PendingSend entries in the queue.
 *
 * Returns the total bytes written, 0 if EAGAIN, -1 on hard error.
 * Updates bufferOffset in each affected PendingSend on success.
 *
 * This collapses N send() syscalls into 1, which is critical for
 * high-frequency small-message workloads (HTTP/1.1, WebSocket frames, etc.)
 */
inline ssize_t gatherAndWriteBuffers(int fd,
                                      std::deque<PendingSend>& queue) noexcept {
    // Build iov array from consecutive Buffer-type head entries.
    iovec iov[kMaxScatterGatherIov];
    size_t iovCount = 0;
    size_t totalBytes = 0;

    for (auto& item : queue) {
        if (item.kind != PendingSendKind::Buffer) break;  // stop at file send
        if (iovCount >= kMaxScatterGatherIov)             break;

        const size_t payloadSize  = item.bufferedPayloadSize();
        const uint8_t* payloadPtr = item.bufferedPayloadBytes();
        if (payloadPtr == nullptr || item.bufferOffset >= payloadSize) break;

        iov[iovCount].iov_base = const_cast<uint8_t*>(payloadPtr + item.bufferOffset);
        iov[iovCount].iov_len  = payloadSize - item.bufferOffset;
        totalBytes            += iov[iovCount].iov_len;
        ++iovCount;
    }

    if (iovCount == 0) return 0;

    const ssize_t written = ::writev(fd, iov, static_cast<int>(iovCount));
    if (written <= 0) { return written; }

    // Commit: advance bufferOffset in each affected PendingSend.
    ssize_t remaining = written;
    for (auto& item : queue) {
        if (remaining <= 0)                          break;
        if (item.kind != PendingSendKind::Buffer)    break;

        const size_t available =
            item.bufferedPayloadSize() - item.bufferOffset;
        if (static_cast<size_t>(remaining) >= available) {
            item.bufferOffset += available;
            remaining         -= static_cast<ssize_t>(available);
        } else {
            item.bufferOffset += static_cast<size_t>(remaining);
            remaining = 0;
        }
    }

    return written;
}
#endif // !_WIN32

#ifdef _WIN32
constexpr DWORD kAcceptExAddressLength = static_cast<DWORD>(sizeof(sockaddr_storage) + 16);
constexpr size_t kMaxWindowsAcceptWorkers = 8;
constexpr size_t kMaxWindowsPendingAccepts = 2048;
constexpr ULONG_PTR kAcceptShutdownCompletionKey = 1;

LPFN_ACCEPTEX loadAcceptEx(socket_t listenFd) {
    GUID extensionGuid = WSAID_ACCEPTEX;
    LPFN_ACCEPTEX acceptEx = nullptr;
    DWORD bytes = 0;
    if (WSAIoctl(listenFd,
                 SIO_GET_EXTENSION_FUNCTION_POINTER,
                 &extensionGuid,
                 static_cast<DWORD>(sizeof(extensionGuid)),
                 &acceptEx,
                 static_cast<DWORD>(sizeof(acceptEx)),
                 &bytes,
                 nullptr,
                 nullptr) != 0) {
        return nullptr;
    }
    return acceptEx;
}

void closeSocketHandle(socket_t& fd) {
    if (fd != INVALID_SOCKET_FD) {
        ::closesocket(fd);
        fd = INVALID_SOCKET_FD;
    }
}

HANDLE createCompletionPort() {
    return ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
}

void closeCompletionPort(HANDLE& handle) {
    if (handle != nullptr) {
        ::CloseHandle(handle);
        handle = nullptr;
    }
}

bool associateSocketWithCompletionPort(HANDLE completionPort, socket_t fd, ULONG_PTR completionKey = 0) {
    return completionPort != nullptr &&
           ::CreateIoCompletionPort(reinterpret_cast<HANDLE>(fd), completionPort, completionKey, 0) != nullptr;
}

socket_t createOverlappedAcceptSocket(int family) {
    return ::WSASocketW(family, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
}

bool isExpectedAcceptShutdownError(int error) noexcept {
    return error == WSA_OPERATION_ABORTED || error == WSAENOTSOCK || error == WSAEINVAL;
}

bool endpointToTextLocal(const sockaddr* address, std::string& ip, uint16_t& port) {
    if (address == nullptr) {
        return false;
    }

    char buffer[INET6_ADDRSTRLEN] = {0};
    if (address->sa_family == AF_INET) {
        const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(address);
        if (::inet_ntop(AF_INET, &ipv4->sin_addr, buffer, static_cast<DWORD>(sizeof(buffer))) == nullptr) {
            return false;
        }
        ip = buffer;
        port = ntohs(ipv4->sin_port);
        return true;
    }

    if (address->sa_family == AF_INET6) {
        const auto* ipv6 = reinterpret_cast<const sockaddr_in6*>(address);
        if (::inet_ntop(AF_INET6, &ipv6->sin6_addr, buffer, static_cast<DWORD>(sizeof(buffer))) == nullptr) {
            return false;
        }
        ip = buffer;
        port = ntohs(ipv6->sin6_port);
        return true;
    }

    return false;
}

bool queryPeerEndpoint(socket_t socketFd, std::string& ip, uint16_t& port) {
    sockaddr_storage remoteAddress{};
    int remoteAddressLength = static_cast<int>(sizeof(remoteAddress));
    if (::getpeername(socketFd,
                      reinterpret_cast<sockaddr*>(&remoteAddress),
                      &remoteAddressLength) != 0) {
        return false;
    }
    return endpointToTextLocal(reinterpret_cast<const sockaddr*>(&remoteAddress), ip, port);
}

bool extractAcceptedRemoteEndpoint(const std::array<std::uint8_t, kAcceptExAddressLength * 2>& addressBuffer,
                                   std::string& ip,
                                   uint16_t& port) {
    sockaddr* localAddress = nullptr;
    sockaddr* remoteAddress = nullptr;
    int localAddressLength = 0;
    int remoteAddressLength = 0;
    ::GetAcceptExSockaddrs(const_cast<std::uint8_t*>(addressBuffer.data()),
                           0,
                           kAcceptExAddressLength,
                           kAcceptExAddressLength,
                           &localAddress,
                           &localAddressLength,
                           &remoteAddress,
                           &remoteAddressLength);
    if (remoteAddress == nullptr || remoteAddressLength <= 0) {
        return false;
    }
    return endpointToTextLocal(remoteAddress, ip, port);
}

struct AcceptContext {
    OVERLAPPED overlapped{};
    socket_t acceptedFd = INVALID_SOCKET_FD;
    std::array<std::uint8_t, kAcceptExAddressLength * 2> addressBuffer{};

    ~AcceptContext() {
        closeSocketHandle(acceptedFd);
    }
};
#elif defined(__linux__)
void configureLinuxListenSocket(socket_t socketFd, int backlog) {
#ifdef TCP_DEFER_ACCEPT
    const int deferAcceptSeconds = 1;
    ::setsockopt(socketFd,
                 IPPROTO_TCP,
                 TCP_DEFER_ACCEPT,
                 reinterpret_cast<const char*>(&deferAcceptSeconds),
                 static_cast<socket_len_t>(sizeof(deferAcceptSeconds)));
#endif
#ifdef TCP_FASTOPEN
    const int fastOpenBacklog = (std::min)(backlog, 256);
    if (fastOpenBacklog > 0) {
        ::setsockopt(socketFd,
                     IPPROTO_TCP,
                     TCP_FASTOPEN,
                     reinterpret_cast<const char*>(&fastOpenBacklog),
                     static_cast<socket_len_t>(sizeof(fastOpenBacklog)));
    }
#endif
}

void configureLinuxAcceptedSocket(socket_t socketFd) {
#ifdef TCP_QUICKACK
    const int quickAckEnabled = 1;
    ::setsockopt(socketFd,
                 IPPROTO_TCP,
                 TCP_QUICKACK,
                 reinterpret_cast<const char*>(&quickAckEnabled),
                 static_cast<socket_len_t>(sizeof(quickAckEnabled)));
#else
    (void)socketFd;
#endif
}
#endif

struct NativeFileHandle {
#ifdef _WIN32
    HANDLE value = INVALID_HANDLE_VALUE;
#else
    int value = -1;
#endif

    NativeFileHandle() = default;
    ~NativeFileHandle() { reset(); }

    NativeFileHandle(const NativeFileHandle&) = delete;
    NativeFileHandle& operator=(const NativeFileHandle&) = delete;

    NativeFileHandle(NativeFileHandle&& other) noexcept
        : value(other.release()) {}

    NativeFileHandle& operator=(NativeFileHandle&& other) noexcept {
        if (this != &other) {
            reset();
            value = other.release();
        }
        return *this;
    }

    bool isValid() const {
#ifdef _WIN32
        return value != nullptr && value != INVALID_HANDLE_VALUE;
#else
        return value >= 0;
#endif
    }

    void reset() {
#ifdef _WIN32
        if (isValid()) {
            CloseHandle(value);
            value = INVALID_HANDLE_VALUE;
        }
#else
        if (isValid()) {
            ::close(value);
            value = -1;
        }
#endif
    }

#ifdef _WIN32
    HANDLE release() {
        HANDLE handle = value;
        value = INVALID_HANDLE_VALUE;
        return handle;
    }
#else
    int release() {
        int handle = value;
        value = -1;
        return handle;
    }
#endif
};

bool openFileForRead(const std::string& path,
                     NativeFileHandle& handle,
                     uint64_t& fileSize,
                     std::string& errorMessage) {
#ifdef _WIN32
    HANDLE file = CreateFileA(path.c_str(),
                              GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr,
                              OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        errorMessage = "Failed to open file: " + path;
        return false;
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0) {
        errorMessage = "Failed to inspect file: " + path;
        CloseHandle(file);
        return false;
    }

    handle.reset();
    handle.value = file;
    fileSize = static_cast<uint64_t>(size.QuadPart);
    return true;
#else
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        errorMessage = "Failed to open file: " + path;
        return false;
    }

    struct stat fileStat {};
    if (fstat(fd, &fileStat) != 0 || fileStat.st_size < 0) {
        errorMessage = "Failed to inspect file: " + path;
        ::close(fd);
        return false;
    }

    handle.reset();
    handle.value = fd;
    fileSize = static_cast<uint64_t>(fileStat.st_size);
    return true;
#endif
}

bool readFileChunk(const NativeFileHandle& handle,
                   uint64_t absoluteOffset,
                   std::uint8_t* buffer,
                   size_t length,
                   size_t& bytesRead,
                   std::string& errorMessage) {
    bytesRead = 0;
    if (length == 0) {
        return true;
    }

#ifdef _WIN32
    LARGE_INTEGER offset {};
    offset.QuadPart = static_cast<LONGLONG>(absoluteOffset);
    if (!SetFilePointerEx(handle.value, offset, nullptr, FILE_BEGIN)) {
        errorMessage = "Failed to seek file";
        return false;
    }

    DWORD readNow = 0;
    if (!ReadFile(handle.value, buffer, static_cast<DWORD>(length), &readNow, nullptr)) {
        errorMessage = "Failed to read file";
        return false;
    }

    bytesRead = static_cast<size_t>(readNow);
    return true;
#else
    const ssize_t readNow = ::pread(handle.value, buffer, length, static_cast<off_t>(absoluteOffset));
    if (readNow < 0) {
        errorMessage = "Failed to read file";
        return false;
    }

    bytesRead = static_cast<size_t>(readNow);
    return true;
#endif
}

enum class PendingSendKind {
    Buffer,
    File
};

// ── PendingSend ───────────────────────────────────────────────────────────────
//
// Hot-path design: every send overload caches (payloadData_, payloadSize_)
// immediately on construction so that the scatter-gather loop and the
// gatherAndWriteBuffers() call only touch a raw pointer + size_t — no
// shared_ptr deref or branch inside the send loop.
//
// Ownership is held by exactly ONE of the four variant members; the others
// remain default-initialised (empty/null).  A union would save memory but
// would require manual lifetime management; given that PendingSend lives
// inside a std::deque whose node allocator already owns the memory, the
// extra ~16 bytes per entry is acceptable.
//
struct PendingSend {
    PendingSendKind kind          = PendingSendKind::Buffer;
    size_t          bufferOffset  = 0;   // bytes already sent from this entry

    // ── Cached view (pointer + size never change after construction) ──────
    const uint8_t* payloadData_ = nullptr;
    size_t         payloadSize_ = 0;

    // ── Ownership variants (exactly one is set per Buffer entry) ─────────
    Buffer                          ownedBuffer;          // move-owns raw bytes
    std::string                     ownedString;          // move-owns string
    std::shared_ptr<const Buffer>   sharedBuffer;         // ref-counted buffer
    std::shared_ptr<const std::string> sharedString;      // ref-counted string

    // ── File-send fields (kind == File) ──────────────────────────────────
    std::string      filePath;
    uint64_t         fileOffset       = 0;
    uint64_t         fileLength       = 0;
    uint64_t         fileBytesSent    = 0;
    bool             directSendEligible = false;
    NativeFileHandle fileHandle;
    Buffer           stagingBuffer;
    size_t           stagingOffset    = 0;

    PendingSend() = default;
    PendingSend(const PendingSend&) = delete;
    PendingSend& operator=(const PendingSend&) = delete;

    PendingSend(PendingSend&& other) noexcept {
        moveFrom(std::move(other));
    }

    PendingSend& operator=(PendingSend&& other) noexcept {
        if (this != &other) {
            fileHandle.reset();
            moveFrom(std::move(other));
        }
        return *this;
    }

    // ── Factory helpers — zero-copy construction with view caching ────────

    // From a copy of a raw Buffer (caller keeps original).
    static PendingSend fromBuffer(const Buffer& buf) {
        PendingSend s;
        s.ownedBuffer  = buf;
        s.payloadData_ = s.ownedBuffer.data();
        s.payloadSize_ = s.ownedBuffer.size();
        return s;
    }

    // From a moved Buffer (zero-copy, no allocation).
    static PendingSend fromBuffer(Buffer&& buf) {
        PendingSend s;
        s.ownedBuffer  = std::move(buf);
        s.payloadData_ = s.ownedBuffer.data();
        s.payloadSize_ = s.ownedBuffer.size();
        return s;
    }

    // From a moved std::string (zero-copy).
    static PendingSend fromString(std::string&& str) {
        PendingSend s;
        s.ownedString  = std::move(str);
        s.payloadData_ = reinterpret_cast<const uint8_t*>(s.ownedString.data());
        s.payloadSize_ = s.ownedString.size();
        return s;
    }

    // From a string_view (must copy — view may not outlive the call).
    static PendingSend fromStringView(std::string_view sv) {
        PendingSend s;
        s.ownedString.assign(sv.data(), sv.size());
        s.payloadData_ = reinterpret_cast<const uint8_t*>(s.ownedString.data());
        s.payloadSize_ = s.ownedString.size();
        return s;
    }

    // From a shared_ptr<const Buffer> — zero-copy, shared ownership.
    static PendingSend fromSharedBuffer(std::shared_ptr<const Buffer> ptr) {
        PendingSend s;
        s.sharedBuffer = std::move(ptr);
        if (s.sharedBuffer && !s.sharedBuffer->empty()) {
            s.payloadData_ = s.sharedBuffer->data();
            s.payloadSize_ = s.sharedBuffer->size();
        }
        return s;
    }

    // From a shared_ptr<const std::string> — zero-copy, shared ownership.
    static PendingSend fromSharedString(std::shared_ptr<const std::string> ptr) {
        PendingSend s;
        s.sharedString = std::move(ptr);
        if (s.sharedString && !s.sharedString->empty()) {
            s.payloadData_ = reinterpret_cast<const uint8_t*>(s.sharedString->data());
            s.payloadSize_ = s.sharedString->size();
        }
        return s;
    }

    // ── O(1) accessors used in the send loop ─────────────────────────────

    // Total bytes in this entry (constant after construction).
    size_t bufferedPayloadSize() const noexcept { return payloadSize_; }

    // Base pointer of payload (constant after construction).
    const uint8_t* bufferedPayloadBytes() const noexcept { return payloadData_; }

    bool isComplete() const noexcept {
        if (kind == PendingSendKind::Buffer) {
            return bufferOffset >= payloadSize_;
        }
        return fileBytesSent >= fileLength;
    }

    uint64_t remainingFileBytes() const noexcept {
        return fileLength > fileBytesSent ? (fileLength - fileBytesSent) : 0;
    }

private:
    void moveFrom(PendingSend&& other) noexcept {
        kind = other.kind;
        bufferOffset = other.bufferOffset;
        ownedBuffer = std::move(other.ownedBuffer);
        ownedString = std::move(other.ownedString);
        sharedBuffer = std::move(other.sharedBuffer);
        sharedString = std::move(other.sharedString);
        filePath = std::move(other.filePath);
        fileOffset = other.fileOffset;
        fileLength = other.fileLength;
        fileBytesSent = other.fileBytesSent;
        directSendEligible = other.directSendEligible;
        fileHandle = std::move(other.fileHandle);
        stagingBuffer = std::move(other.stagingBuffer);
        stagingOffset = other.stagingOffset;
        refreshPayloadView();
        other.payloadData_ = nullptr;
        other.payloadSize_ = 0;
    }

    void refreshPayloadView() noexcept {
        payloadData_ = nullptr;
        payloadSize_ = 0;
        if (kind != PendingSendKind::Buffer) {
            return;
        }
        if (!ownedBuffer.empty()) {
            payloadData_ = ownedBuffer.data();
            payloadSize_ = ownedBuffer.size();
        } else if (!ownedString.empty()) {
            payloadData_ = reinterpret_cast<const uint8_t*>(ownedString.data());
            payloadSize_ = ownedString.size();
        } else if (sharedBuffer && !sharedBuffer->empty()) {
            payloadData_ = sharedBuffer->data();
            payloadSize_ = sharedBuffer->size();
        } else if (sharedString && !sharedString->empty()) {
            payloadData_ = reinterpret_cast<const uint8_t*>(sharedString->data());
            payloadSize_ = sharedString->size();
        }
    }
};

enum class FlushOutcome {
    QueueDrained,
    WouldBlock,
    Closed
};


} // namespace

class ClientSession : public std::enable_shared_from_this<ClientSession>
#ifdef _WIN32
                    , public WindowsIocpSocketHandler
#endif
{
public:
    ClientSession(IoService& ioService,
                  socket_t fd,
                  ConnectionId id,
                  const Address& clientAddress,
                  SSLContext* sslContext,
                  const SessionTimeoutConfig& timeoutConfig)
        : ioService_(ioService),
          socket_(fd),
          id_(id),
          clientAddress_(clientAddress),
          sslContext_(sslContext),
          timeoutConfig_(timeoutConfig),
          connectionTimer_(std::make_unique<Timer>(ioService)),
          readTimer_(std::make_unique<Timer>(ioService)),
          writeTimer_(std::make_unique<Timer>(ioService)) {}

    ~ClientSession() {
        close("Connection closed");
    }

    void setCallbacks(const std::function<void(ConnectionId, const Address&)>& readyCallback,
                      const ServerDataReceivedCallback& dataCallback,
                      const ServerDataReceivedOwnedCallback& ownedDataCallback,
                      const ServerDataReceivedSharedCallback& sharedDataCallback,
                      const std::function<void(ConnectionId, bool, const std::string&)>& closedCallback,
                      const ServerErrorCallback& errorCallback) {
        std::lock_guard<std::recursive_mutex> callbackLock(callbackMutex_);
        readyCallback_ = readyCallback;
        dataCallback_ = dataCallback;
        ownedDataCallback_ = ownedDataCallback;
        sharedDataCallback_ = sharedDataCallback;
        closedCallback_ = closedCallback;
        errorCallback_ = errorCallback;
    }

    bool start() {
        Error result = socket_.setNoDelay(true);
        if (result.isFailure()) {
            lastFailureCode_ = result.getCode();
            lastFailureMessage_ = result.getMessage();
            socket_.close();
            return false;
        }

        result = socket_.setKeepAlive(true);
        if (result.isFailure()) {
            lastFailureCode_ = result.getCode();
            lastFailureMessage_ = result.getMessage();
            socket_.close();
            return false;
        }
        socket_.optimizeLoopbackFastPath();

#ifdef __linux__
        configureLinuxAcceptedSocket(socket_.getFd());
#endif

#ifdef _WIN32
        running_.store(true, std::memory_order_release);
        windowsTransport_ = WindowsIocpSocketTransport::create(
            socket_.getFd(),
            std::static_pointer_cast<WindowsIocpSocketHandler>(shared_from_this()),
            ioService_.getThreadCount(),
            16384);
        if (!windowsTransport_) {
            lastFailureCode_ = ErrorCode::SocketError;
            lastFailureMessage_ = "Failed to create Windows IOCP transport";
            socket_.close();
            running_.store(false, std::memory_order_release);
            return false;
        }

        result = windowsTransport_->start();
        if (result.isFailure()) {
            auto transport = std::move(windowsTransport_);
            if (transport) {
                transport->stop();
            }
            lastFailureCode_ = result.getCode();
            lastFailureMessage_ = result.toString();
            socket_.close();
            running_.store(false, std::memory_order_release);
            return false;
        }

        armConnectionTimer();
        if (sslContext_ != nullptr) {
            if (!initializeTls()) {
                abortStartup();
                return false;
            }
            if (!driveTlsHandshake()) {
                abortStartup();
                return false;
            }
        } else {
            markReady();
        }
        return true;
#endif

        result = socket_.setNonBlocking(true);
        if (result.isFailure()) {
            lastFailureCode_ = result.getCode();
            lastFailureMessage_ = result.getMessage();
            socket_.close();
            return false;
        }

        if (!ioService_.getPoller().add(
                socket_.getFd(),
                EventType::Read | EventType::Error | EventType::Close,
                [self = shared_from_this()](socket_t, EventType events, void*, size_t) {
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
            lastFailureCode_ = ErrorCode::SocketError;
            lastFailureMessage_ = "Failed to register client socket with poller";
            socket_.close();
            return false;
        }

        running_.store(true, std::memory_order_release);
        armConnectionTimer();

        if (sslContext_ != nullptr) {
            if (!initializeTls()) {
                abortStartup();
                return false;
            }
            if (!driveTlsHandshake()) {
                abortStartup();
                return false;
            }
        } else {
            markReady();
        }

        return true;
    }

    bool send(const Buffer& data) {
        if (!ready_.load(std::memory_order_acquire)) { return false; }
        if (data.empty()) { return true; }

        bool shouldScheduleFlush = false;
        {
            std::lock_guard<std::mutex> lock(sendMutex_);
            sendQueue_.push_back(PendingSend::fromBuffer(data));
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
        if (!ready_.load(std::memory_order_acquire)) { return false; }
        if (data.empty()) { return true; }

        bool shouldScheduleFlush = false;
        {
            std::lock_guard<std::mutex> lock(sendMutex_);
            sendQueue_.push_back(PendingSend::fromBuffer(std::move(data)));
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
        if (!ready_.load(std::memory_order_acquire)) { return false; }
        if (data.empty()) { return true; }

        bool shouldScheduleFlush = false;
        {
            std::lock_guard<std::mutex> lock(sendMutex_);
            sendQueue_.push_back(PendingSend::fromString(std::move(data)));
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
        if (!ready_.load(std::memory_order_acquire)) { return false; }
        if (data.empty()) { return true; }

        bool shouldScheduleFlush = false;
        {
            std::lock_guard<std::mutex> lock(sendMutex_);
            sendQueue_.push_back(PendingSend::fromStringView(data));
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

    bool sendSharedBuffer(const std::shared_ptr<const Buffer>& data) {
        if (!ready_.load(std::memory_order_acquire)) { return false; }
        if (!data || data->empty()) { return true; }

        bool shouldScheduleFlush = false;
        {
            std::lock_guard<std::mutex> lock(sendMutex_);
            sendQueue_.push_back(PendingSend::fromSharedBuffer(data));
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

    bool sendSharedString(const std::shared_ptr<const std::string>& data) {
        if (!ready_.load(std::memory_order_acquire)) { return false; }
        if (!data || data->empty()) { return true; }

        bool shouldScheduleFlush = false;
        {
            std::lock_guard<std::mutex> lock(sendMutex_);
            sendQueue_.push_back(PendingSend::fromSharedString(data));
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

    Error sendFile(const Buffer& prefix,
                   const std::string& filePath,
                   uint64_t offset,
                   uint64_t length) {
        return sendFile(prefix.empty()
                            ? std::string_view()
                            : std::string_view(reinterpret_cast<const char*>(prefix.data()), prefix.size()),
                        filePath,
                        offset,
                        length);
    }

    Error sendFile(std::string_view prefix,
                   const std::string& filePath,
                   uint64_t offset,
                   uint64_t length) {
        std::string ownedPrefix(prefix);
        return sendFile(std::move(ownedPrefix), filePath, offset, length);
    }

    Error sendFile(std::string&& prefix,
                   const std::string& filePath,
                   uint64_t offset,
                   uint64_t length) {
        if (!ready_.load(std::memory_order_acquire)) {
            return FASTNET_ERROR(ErrorCode::ConnectionError, "Client connection is not ready");
        }
        if (filePath.empty()) {
            return FASTNET_ERROR(ErrorCode::InvalidArgument, "File path must not be empty");
        }

        PendingSend fileSend;
        fileSend.kind = PendingSendKind::File;
        fileSend.filePath = filePath;
        fileSend.fileOffset = offset;

        std::string openFailure;
        uint64_t fileSize = 0;
        if (!openFileForRead(filePath, fileSend.fileHandle, fileSize, openFailure)) {
            return FASTNET_ERROR(ErrorCode::UnknownError, openFailure);
        }
        if (offset > fileSize) {
            return FASTNET_ERROR(ErrorCode::InvalidArgument, "Requested file offset is past the end of file");
        }

        const uint64_t remaining = fileSize - offset;
        fileSend.fileLength = (length == 0) ? remaining : length;
        if (fileSend.fileLength > remaining) {
            return FASTNET_ERROR(ErrorCode::InvalidArgument, "Requested file range exceeds file size");
        }

#ifdef _WIN32
        fileSend.directSendEligible = false;
#else
        fileSend.directSendEligible = (tlsState_ == TlsState::Disabled);
#endif
        const bool hasFileBody = fileSend.fileLength > 0;
        if (prefix.empty() && !hasFileBody) {
            return FASTNET_SUCCESS;
        }

        bool shouldScheduleFlush = false;
        {
            std::lock_guard<std::mutex> lock(sendMutex_);
            if (!prefix.empty()) {
                sendQueue_.push_back(PendingSend::fromString(std::move(prefix)));
            }
            if (hasFileBody) {
                sendQueue_.push_back(std::move(fileSend));
            }
#ifdef _WIN32
            if (usingWindowsIocpTransport()) {
                shouldScheduleFlush = !iocpWritePending_;
            } else
#endif
            {
                refreshInterestLocked();
                shouldScheduleFlush = markFlushScheduledLocked();
            }
        }

        if (!prefix.empty() || hasFileBody) {
            armWriteTimer();
        }
        dispatchFlushIfNeeded(shouldScheduleFlush);
        return FASTNET_SUCCESS;
    }

    void closeAfterPendingWrites() {
        bool shouldCloseNow = false;
        {
            std::lock_guard<std::mutex> lock(sendMutex_);
            closeAfterFlush_ = true;
            bool hasPendingOutput = !sendQueue_.empty();
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
            close("Connection closed");
        }
    }

    void close(const std::string& reason) {
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
            ready_.store(false, std::memory_order_release);
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

        const bool wasReady = ready_.exchange(false, std::memory_order_acq_rel);
        stopAllTimers();
        clearQueuesAndWaits();
        releaseTls();
        socket_.close();

        if (closedCallback_) {
            closedCallback_(id_, wasReady, reason);
        }
    }

    ConnectionId id() const { return id_; }
    Address address() const { return clientAddress_; }
    bool isReady() const { return ready_.load(std::memory_order_acquire); }
    ErrorCode getLastFailureCode() const { return lastFailureCode_; }
    const std::string& getLastFailureMessage() const { return lastFailureMessage_; }

    void updateTimeouts(const SessionTimeoutConfig& timeoutConfig) {
        timeoutConfig_ = timeoutConfig;

        if (timeoutConfig_.connectionTimeoutMs == 0) {
            stopConnectionTimer();
        } else if (running_.load(std::memory_order_acquire)) {
            armConnectionTimer();
        }

        if (timeoutConfig_.readTimeoutMs == 0) {
            stopReadTimer();
        } else if (ready_.load(std::memory_order_acquire)) {
            armReadTimer();
        }

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
        if (timeoutConfig_.writeTimeoutMs == 0 || !hasPendingWrite) {
            stopWriteTimer();
        } else if (ready_.load(std::memory_order_acquire)) {
            armWriteTimer();
        }
    }

private:
    void abortStartup() {
        std::lock_guard<std::recursive_mutex> callbackLock(callbackMutex_);
        running_.store(false, std::memory_order_release);
        ready_.store(false, std::memory_order_release);
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

    void markReady() {
        if (ready_.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        armConnectionTimer();
        armReadTimer();
        if (readyCallback_) {
            readyCallback_(id_, clientAddress_);
        }
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

#ifdef _WIN32
    void retainOutstandingWindowsIocpWriteBuffersLocked() {
        if (!iocpWritePending_) {
            return;
        }
        if (!sendQueue_.empty()) {
            retainedSendBatches_.push_back(std::make_unique<std::deque<PendingSend>>(std::move(sendQueue_)));
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

    void handleRead() {
        std::lock_guard<std::recursive_mutex> callbackLock(callbackMutex_);
        if (!running_.load(std::memory_order_acquire)) {
            return;
        }

        if (tlsState_ == TlsState::Handshaking) {
            if (!driveTlsHandshake()) {
                handleTransportFailure(lastFailureCode_, lastFailureMessage_);
            }
            return;
        }

        constexpr size_t kReadChunkSize = 16384;
        while (running_.load(std::memory_order_acquire)) {
            std::shared_ptr<Buffer> sharedPayloadBuffer;
            Buffer directPayloadBuffer;
            void* readTarget = nullptr;
            size_t readCapacity = kReadChunkSize;
            std::array<std::uint8_t, kReadChunkSize> stackBuffer;
            const bool useDirectPayloadBuffer = ownedDataCallback_ || dataCallback_;
            if (sharedDataCallback_) {
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
                armConnectionTimer();
                armReadTimer();
                if (sharedDataCallback_) {
                    sharedPayloadBuffer->resize(result.bytes);
                    std::shared_ptr<const Buffer> sharedPayload = sharedPayloadBuffer;
                    sharedDataCallback_(id_, sharedPayload);
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
                close("Connection closed by peer");
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
                handleTransportFailure(lastFailureCode_, lastFailureMessage_);
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
        while (ready_.load(std::memory_order_acquire)) {
            const FlushOutcome outcome = drainSendQueue();

            bool shouldContinue = false;
            {
                std::lock_guard<std::mutex> lock(sendMutex_);
                if (!running_.load(std::memory_order_acquire) || outcome != FlushOutcome::QueueDrained) {
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

    FlushOutcome drainSendQueue() {
        while (ready_.load(std::memory_order_acquire)) {
            PendingSend* current = nullptr;
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
                    current = &sendQueue_.front();
                }
            }

            if (shouldCloseAfterFlush) {
                close("Connection closed");
                return FlushOutcome::Closed;
            }
            if (!current) {
                return FlushOutcome::QueueDrained;
            }

            if (current->isComplete()) {
                bool closeNow = false;
                {
                    std::lock_guard<std::mutex> lock(sendMutex_);
                    if (!sendQueue_.empty() && &sendQueue_.front() == current) {
                        sendQueue_.pop_front();
                    }
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
                    close("Connection closed");
                    return FlushOutcome::Closed;
                }
                continue;
            }

            const IoResult result = writePendingSend(*current);
            if (result.status == IoStatus::Ok) {
                setWriteWait(WaitDirection::None);
                armConnectionTimer();

                bool closeNow = false;
                {
                    std::lock_guard<std::mutex> lock(sendMutex_);
                    if (!sendQueue_.empty() && &sendQueue_.front() == current && current->isComplete()) {
                        sendQueue_.pop_front();
                    }
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
                    close("Connection closed");
                    return FlushOutcome::Closed;
                }
                continue;
            }

            if (result.status == IoStatus::WouldBlock) {
                armWriteTimer();
                setWriteWait(result.wait);
                return FlushOutcome::WouldBlock;
            }

            if (result.status == IoStatus::Closed) {
                close("Connection closed by peer");
                return FlushOutcome::Closed;
            }

            handleTransportFailure(result.errorCode, result.message);
            return FlushOutcome::Closed;
        }

        return FlushOutcome::Closed;
    }

    IoResult writePendingSend(PendingSend& sendItem) {
        if (sendItem.kind == PendingSendKind::Buffer) {
            const size_t payloadSize = sendItem.bufferedPayloadSize();
            const std::uint8_t* payloadBytes = sendItem.bufferedPayloadBytes();
            if (sendItem.bufferOffset >= payloadSize || payloadBytes == nullptr) {
                return {IoStatus::Ok, 0};
            }

            const IoResult result = writeTransport(payloadBytes + sendItem.bufferOffset,
                                                   payloadSize - sendItem.bufferOffset);
            if (result.status == IoStatus::Ok) {
                sendItem.bufferOffset += result.bytes;
            }
            return result;
        }

        return writeFilePendingSend(sendItem);
    }

    IoResult writeFilePendingSend(PendingSend& sendItem) {
        if (sendItem.remainingFileBytes() == 0) {
            return {IoStatus::Ok, 0};
        }

        if (sendItem.directSendEligible) {
#ifdef _WIN32
            sendItem.directSendEligible = false;
#else
            off_t absoluteOffset = static_cast<off_t>(sendItem.fileOffset + sendItem.fileBytesSent);
            const size_t chunkSize = static_cast<size_t>(std::min<uint64_t>(
                sendItem.remainingFileBytes(),
                static_cast<uint64_t>(kDirectFileSendChunkSize)));
            const ssize_t sent = socket_.sendFile(sendItem.fileHandle.value, absoluteOffset, chunkSize);
            if (sent > 0) {
                sendItem.fileBytesSent += static_cast<uint64_t>(sent);
                return {IoStatus::Ok, static_cast<size_t>(sent)};
            }
            if (sent == 0) {
                return {IoStatus::Error, 0, WaitDirection::None, ErrorCode::UnknownError,
                        "Reached end of file before the configured range was sent"};
            }

            const int error = errno;
            if (isSocketWouldBlock(error)) {
                return {IoStatus::WouldBlock, 0, WaitDirection::Write};
            }
            return {IoStatus::Error, 0, WaitDirection::None, ErrorCode::SocketError,
                    SocketWrapper::getErrorMessage(error)};
#endif
        }

        if (sendItem.stagingOffset >= sendItem.stagingBuffer.size()) {
            const size_t chunkSize = static_cast<size_t>(std::min<uint64_t>(
                sendItem.remainingFileBytes(),
                static_cast<uint64_t>(kBufferedFileChunkSize)));
            sendItem.stagingBuffer.resize(chunkSize);

            size_t bytesRead = 0;
            std::string readError;
            if (!readFileChunk(sendItem.fileHandle,
                               sendItem.fileOffset + sendItem.fileBytesSent,
                               sendItem.stagingBuffer.data(),
                               chunkSize,
                               bytesRead,
                               readError)) {
                sendItem.stagingBuffer.clear();
                sendItem.stagingOffset = 0;
                return {IoStatus::Error, 0, WaitDirection::None, ErrorCode::UnknownError, readError};
            }

            if (bytesRead == 0) {
                sendItem.stagingBuffer.clear();
                sendItem.stagingOffset = 0;
                return {IoStatus::Error, 0, WaitDirection::None, ErrorCode::UnknownError,
                        "Reached end of file before the configured range was sent"};
            }

            sendItem.stagingBuffer.resize(bytesRead);
            sendItem.stagingOffset = 0;
        }

        const IoResult result = writeTransport(sendItem.stagingBuffer.data() + sendItem.stagingOffset,
                                               sendItem.stagingBuffer.size() - sendItem.stagingOffset);
        if (result.status == IoStatus::Ok) {
            sendItem.stagingOffset += result.bytes;
            sendItem.fileBytesSent += result.bytes;
            if (sendItem.stagingOffset >= sendItem.stagingBuffer.size()) {
                sendItem.stagingBuffer.clear();
                sendItem.stagingOffset = 0;
            }
        }
        return result;
    }

    void handleTransportFailure(ErrorCode code, const std::string& message) {
        if (errorCallback_) {
            errorCallback_(Error(code, message));
        }
        close(message);
    }

    void dispatchReceivedBuffer(Buffer&& payload) {
        if (payload.empty()) {
            return;
        }

        if (ownedDataCallback_) {
            ownedDataCallback_(id_, std::move(payload));
            return;
        }

        if (dataCallback_) {
            dataCallback_(id_, payload);
        }
    }

    void processReceivedData(const std::uint8_t* data, size_t size) {
        if (data == nullptr || size == 0) {
            return;
        }

        if (ownedDataCallback_ || dataCallback_) {
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

#ifdef FASTNET_ENABLE_SSL
    bool usingWindowsIocpTls() const {
        return usingWindowsIocpTransport() && sslHandle_ != nullptr;
    }

    bool hasPendingWindowsIocpTlsOutputLocked() const {
        return iocpWritePending_ || !tlsWriteQueue_.empty();
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
        constexpr size_t kReadChunkSize = 16384;

        while (running_.load(std::memory_order_acquire) && tlsState_ == TlsState::Ready) {
            std::shared_ptr<Buffer> sharedPayloadBuffer;
            Buffer directPayloadBuffer;
            std::array<std::uint8_t, kReadChunkSize> stackBuffer{};
            std::uint8_t* readTarget = nullptr;
            size_t readCapacity = kReadChunkSize;
            const bool useDirectPayloadBuffer = ownedDataCallback_ || dataCallback_;
            if (sharedDataCallback_) {
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
                armConnectionTimer();
                armReadTimer();
                if (sharedDataCallback_) {
                    sharedPayloadBuffer->resize(static_cast<size_t>(received));
                    std::shared_ptr<const Buffer> sharedPayload = sharedPayloadBuffer;
                    sharedDataCallback_(id_, sharedPayload);
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
                        lastFailureCode_ = ErrorCode::SSLError;
                        lastFailureMessage_ = "Failed to flush pending TLS ciphertext";
                        return false;
                    }
                    return true;
                case SSL_ERROR_ZERO_RETURN:
                    close("Connection closed by peer");
                    return false;
                default:
                    lastFailureCode_ = ErrorCode::SSLError;
                    lastFailureMessage_ = formatOpenSslFailure("TLS read failed");
                    return false;
            }
        }

        return true;
    }
#endif

    Error prepareNextIocpWrite(const std::uint8_t*& payload, size_t& payloadSize, bool& closeNow) {
        payload = nullptr;
        payloadSize = 0;
        closeNow = false;

        constexpr size_t kMaxIocpWriteSize = (std::numeric_limits<DWORD>::max)();

        std::lock_guard<std::mutex> lock(sendMutex_);
        if (!running_.load(std::memory_order_acquire) || !windowsTransport_ || iocpWritePending_) {
            return FASTNET_SUCCESS;
        }

#ifdef FASTNET_ENABLE_SSL
        if (usingWindowsIocpTls()) {
            if (!tlsWriteQueue_.empty()) {
                Buffer& encryptedChunk = tlsWriteQueue_.front();
                if (tlsWriteOffset_ >= encryptedChunk.size()) {
                    tlsWriteQueue_.pop_front();
                    tlsWriteOffset_ = 0;
                    return FASTNET_SUCCESS;
                }

                payload = encryptedChunk.data() + tlsWriteOffset_;
                payloadSize = (std::min)(encryptedChunk.size() - tlsWriteOffset_, kMaxIocpWriteSize);
                iocpWritePending_ = true;
                return FASTNET_SUCCESS;
            }

            if (sendQueue_.empty()) {
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
#endif

        while (!sendQueue_.empty()) {
            PendingSend& current = sendQueue_.front();
            if (current.kind == PendingSendKind::Buffer) {
                const size_t currentSize = current.bufferedPayloadSize();
                const std::uint8_t* currentBytes = current.bufferedPayloadBytes();
                if (current.bufferOffset >= currentSize || currentBytes == nullptr) {
                    sendQueue_.pop_front();
                    continue;
                }

                payload = currentBytes + current.bufferOffset;
                payloadSize = (std::min)(currentSize - current.bufferOffset, kMaxIocpWriteSize);
                iocpWritePending_ = true;
                return FASTNET_SUCCESS;
            }

            if (current.remainingFileBytes() == 0) {
                sendQueue_.pop_front();
                continue;
            }

            if (current.stagingOffset >= current.stagingBuffer.size()) {
                const size_t chunkSize = static_cast<size_t>(std::min<uint64_t>(
                    current.remainingFileBytes(),
                    static_cast<uint64_t>(kBufferedFileChunkSize)));
                current.stagingBuffer.resize(chunkSize);

                size_t bytesRead = 0;
                std::string readError;
                if (!readFileChunk(current.fileHandle,
                                   current.fileOffset + current.fileBytesSent,
                                   current.stagingBuffer.data(),
                                   chunkSize,
                                   bytesRead,
                                   readError)) {
                    current.stagingBuffer.clear();
                    current.stagingOffset = 0;
                    return FASTNET_ERROR(ErrorCode::UnknownError, readError);
                }

                if (bytesRead == 0) {
                    current.stagingBuffer.clear();
                    current.stagingOffset = 0;
                    return FASTNET_ERROR(ErrorCode::UnknownError,
                                         "Reached end of file before the configured range was sent");
                }

                current.stagingBuffer.resize(bytesRead);
                current.stagingOffset = 0;
            }

            payload = current.stagingBuffer.data() + current.stagingOffset;
            payloadSize = (std::min)(current.stagingBuffer.size() - current.stagingOffset, kMaxIocpWriteSize);
            iocpWritePending_ = true;
            return FASTNET_SUCCESS;
        }

        stopWriteTimer();
        flushRunning_ = false;
        flushReschedule_ = false;
        closeNow = closeAfterFlush_;
        if (closeNow) {
            closeAfterFlush_ = false;
        }
        return FASTNET_SUCCESS;
    }

    void startIocpWriteIfNeeded() {
        std::shared_ptr<WindowsIocpSocketTransport> transport;
        const std::uint8_t* payload = nullptr;
        size_t payloadSize = 0;
        bool closeNow = false;
        Error prepareError = FASTNET_SUCCESS;

        transport = windowsTransport_;
        if (!transport) {
            return;
        }

        prepareError = prepareNextIocpWrite(payload, payloadSize, closeNow);
        if (prepareError.isFailure()) {
            handleTransportFailure(prepareError.getCode(), prepareError.toString());
            return;
        }

        if (closeNow) {
            close("Connection closed");
            return;
        }
#ifdef FASTNET_ENABLE_SSL
        if (usingWindowsIocpTls() && payload == nullptr && payloadSize == 0) {
            if (tlsState_ == TlsState::Ready && ready_.load(std::memory_order_acquire)) {
                runQueuedFlush();
            }
            return;
        }
#endif
        if (payload == nullptr || payloadSize == 0) {
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
                    handleTransportFailure(lastFailureCode_, lastFailureMessage_);
                    return;
                }

                if (tlsState_ != TlsState::Ready) {
                    return;
                }

                if (!pumpWindowsIocpTlsPlaintext()) {
                    handleTransportFailure(lastFailureCode_, lastFailureMessage_);
                }
            }

            if (!pumpWindowsIocpTlsPlaintext()) {
                handleTransportFailure(lastFailureCode_, lastFailureMessage_);
                return;
            }

            bool shouldResumeTlsWrite = false;
            {
                std::lock_guard<std::mutex> lock(sendMutex_);
                shouldResumeTlsWrite = writeWait_ == WaitDirection::Read;
            }
            if (shouldResumeTlsWrite && ready_.load(std::memory_order_acquire)) {
                runQueuedFlush();
            }
            return;
        }
#endif
        armConnectionTimer();
        armReadTimer();
        if (sharedDataCallback_) {
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
            sharedDataCallback_(id_, sharedPayload);
            return;
        }
        processReceivedData(data, size);
    }

    void handleWindowsIocpReadClosed() override {
        std::lock_guard<std::recursive_mutex> callbackLock(callbackMutex_);
        if (running_.load(std::memory_order_acquire)) {
            close("Connection closed by peer");
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

                const bool hasAppData = !sendQueue_.empty();
                const bool hasEncryptedData = !tlsWriteQueue_.empty();
                if (!hasAppData && !hasEncryptedData) {
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
                    PendingSend& current = sendQueue_.front();
                    if (current.kind == PendingSendKind::Buffer) {
                        current.bufferOffset += bytesTransferred;
                        if (current.bufferOffset >= current.bufferedPayloadSize()) {
                            sendQueue_.pop_front();
                        }
                    } else {
                        current.stagingOffset += bytesTransferred;
                        current.fileBytesSent += bytesTransferred;
                        if (current.stagingOffset >= current.stagingBuffer.size()) {
                            current.stagingBuffer.clear();
                            current.stagingOffset = 0;
                        }
                        if (current.fileBytesSent >= current.fileLength) {
                            sendQueue_.pop_front();
                        }
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

        armConnectionTimer();
#ifdef FASTNET_ENABLE_SSL
        if (usingWindowsIocpTls() && tlsState_ == TlsState::Handshaking) {
            if (!driveTlsHandshake()) {
                handleTransportFailure(lastFailureCode_, lastFailureMessage_);
                return;
            }
        }

        bool shouldResumeTlsRead = false;
        if (usingWindowsIocpTls() && tlsState_ == TlsState::Ready) {
            std::lock_guard<std::mutex> lock(sendMutex_);
            shouldResumeTlsRead = readWait_ == WaitDirection::Write;
        }
        if (usingWindowsIocpTls() && tlsState_ == TlsState::Ready && shouldResumeTlsRead) {
            if (!pumpWindowsIocpTlsPlaintext()) {
                handleTransportFailure(lastFailureCode_, lastFailureMessage_);
                return;
            }
        }
#endif
        if (closeNow) {
            close("Connection closed");
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

    void armConnectionTimer() {
        if (!connectionTimer_ || timeoutConfig_.connectionTimeoutMs == 0) {
            return;
        }

        std::weak_ptr<ClientSession> weakSelf = shared_from_this();
        connectionTimer_->start(std::chrono::milliseconds(timeoutConfig_.connectionTimeoutMs), [weakSelf]() {
            auto self = weakSelf.lock();
            if (!self || !self->running_.load(std::memory_order_acquire)) {
                return;
            }
            self->handleTransportFailure(ErrorCode::TimeoutError, "Connection timed out");
        });
    }

    void stopConnectionTimer() {
        if (connectionTimer_) {
            connectionTimer_->stop();
        }
    }

    void armReadTimer() {
        if (!readTimer_ ||
            timeoutConfig_.readTimeoutMs == 0 ||
            !ready_.load(std::memory_order_acquire)) {
            return;
        }

        std::weak_ptr<ClientSession> weakSelf = shared_from_this();
        readTimer_->start(std::chrono::milliseconds(timeoutConfig_.readTimeoutMs), [weakSelf]() {
            auto self = weakSelf.lock();
            if (!self || !self->ready_.load(std::memory_order_acquire)) {
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
        if (!writeTimer_ ||
            timeoutConfig_.writeTimeoutMs == 0 ||
            !ready_.load(std::memory_order_acquire)) {
            return;
        }

        std::weak_ptr<ClientSession> weakSelf = shared_from_this();
        writeTimer_->start(std::chrono::milliseconds(timeoutConfig_.writeTimeoutMs), [weakSelf]() {
            auto self = weakSelf.lock();
            if (!self || !self->ready_.load(std::memory_order_acquire)) {
                return;
            }

            bool hasPendingWrite = false;
            {
                std::lock_guard<std::mutex> lock(self->sendMutex_);
                hasPendingWrite = !self->sendQueue_.empty();
#ifdef FASTNET_ENABLE_SSL
                if (self->usingWindowsIocpTls()) {
                    hasPendingWrite = hasPendingWrite || self->hasPendingWindowsIocpTlsOutputLocked();
                }
#endif
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
        stopConnectionTimer();
        stopReadTimer();
        stopWriteTimer();
    }

    bool initializeTls() {
#ifndef FASTNET_ENABLE_SSL
        lastFailureCode_ = ErrorCode::SSLError;
        lastFailureMessage_ = "SSL requested but FastNet was built without FASTNET_ENABLE_SSL";
        return false;
#else
        std::lock_guard<std::recursive_mutex> tlsLock(tlsMutex_);
        sslHandle_ = sslContext_->createHandle();
        if (sslHandle_ == nullptr) {
            lastFailureCode_ = ErrorCode::SSLError;
            lastFailureMessage_ = formatOpenSslFailure("Failed to create SSL handle");
            return false;
        }

#ifdef _WIN32
        if (usingWindowsIocpTransport()) {
            sslReadBio_ = BIO_new(BIO_s_mem());
            sslWriteBio_ = BIO_new(BIO_s_mem());
            if (sslReadBio_ == nullptr || sslWriteBio_ == nullptr) {
                lastFailureCode_ = ErrorCode::SSLError;
                lastFailureMessage_ = formatOpenSslFailure("Failed to create TLS memory BIOs");
                releaseTls();
                return false;
            }
            SSL_set_bio(sslHandle_, sslReadBio_, sslWriteBio_);
        } else
#endif
        {
            if (SSL_set_fd(sslHandle_, static_cast<int>(socket_.getFd())) != 1) {
                lastFailureCode_ = ErrorCode::SSLError;
                lastFailureMessage_ = formatOpenSslFailure("Failed to bind SSL handle to socket");
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
        lastFailureCode_ = ErrorCode::SSLError;
        lastFailureMessage_ = "SSL requested but FastNet was built without FASTNET_ENABLE_SSL";
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
                if (sslContext_->getConfig().verifyPeer) {
                    const long verifyResult = SSL_get_verify_result(sslHandle_);
                    if (verifyResult != X509_V_OK) {
                        lastFailureCode_ = ErrorCode::SSLCertificateError;
                        lastFailureMessage_ = formatVerifyFailure(verifyResult);
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
                        lastFailureCode_ = ErrorCode::SSLHandshakeError;
                        lastFailureMessage_ = "TLS handshake aborted by peer";
                        return false;
                    default:
                        lastFailureCode_ = ErrorCode::SSLHandshakeError;
                        lastFailureMessage_ = formatOpenSslFailure("TLS handshake failed");
                        return false;
                }
            }
        }

#ifdef _WIN32
        if (usingWindowsIocpTransport() && shouldFlushPendingCiphertext) {
            const Error flushError = flushPendingWindowsIocpTlsCiphertext();
            if (flushError.isFailure()) {
                lastFailureCode_ = flushError.getCode();
                lastFailureMessage_ = flushError.toString();
                return false;
            }
        }
#endif
        if (handshakeCompleted) {
            setHandshakeWait(WaitDirection::None);
            markReady();
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
        tlsState_ = TlsState::Disabled;
    }

    IoService& ioService_;
    SocketWrapper socket_;
    ConnectionId id_;
    Address clientAddress_;
    SSLContext* sslContext_ = nullptr;
    SessionTimeoutConfig timeoutConfig_;
    std::atomic<bool> running_{false};
    std::atomic<bool> ready_{false};
    mutable std::recursive_mutex callbackMutex_;

    std::function<void(ConnectionId, const Address&)> readyCallback_;
    ServerDataReceivedCallback dataCallback_;
    ServerDataReceivedOwnedCallback ownedDataCallback_;
    ServerDataReceivedSharedCallback sharedDataCallback_;
    std::function<void(ConnectionId, bool, const std::string&)> closedCallback_;
    ServerErrorCallback errorCallback_;

    std::mutex sendMutex_;
    std::deque<PendingSend> sendQueue_;
    bool closeAfterFlush_ = false;
    bool flushRunning_ = false;
    bool flushReschedule_ = false;
    bool iocpWritePending_ = false;
    bool writeInterestEnabled_ = false;
    WaitDirection readWait_ = WaitDirection::None;
    WaitDirection writeWait_ = WaitDirection::None;
    WaitDirection handshakeWait_ = WaitDirection::None;
    TlsState tlsState_ = TlsState::Disabled;
    ErrorCode lastFailureCode_ = ErrorCode::Success;
    std::string lastFailureMessage_;
    std::unique_ptr<Timer> connectionTimer_;
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
    std::vector<std::unique_ptr<std::deque<PendingSend>>> retainedSendBatches_;
#ifdef FASTNET_ENABLE_SSL
    std::vector<std::unique_ptr<std::deque<Buffer>>> retainedTlsWriteBatches_;
#endif
#endif
};

class TcpServer::Impl {
public:
    explicit Impl(IoService& ioService)
        : ioService_(ioService) {}

    ~Impl() {
        stop();
    }

    Error start(uint16_t port, const std::string& bindAddress, const SSLConfig& sslConfig) {
        if (running_.exchange(true, std::memory_order_acq_rel)) {
            return FASTNET_ERROR(ErrorCode::AlreadyRunning, "Server is already running");
        }

        sslConfig_ = sslConfig;
        sslEnabled_ = sslConfig.enableSSL;

        if (sslEnabled_) {
            if (!sslContext_.initialize(sslConfig_, SSLContext::Mode::Server)) {
                running_.store(false, std::memory_order_release);
                sslEnabled_ = false;
                return Error(ErrorCode::SSLError, sslContext_.getLastErrorString());
            }
        }

        Error error = listenSocket_.create(SocketType::TCP, bindAddress, port, true);
        if (error.isFailure()) {
            cleanupServerTls();
            running_.store(false, std::memory_order_release);
            return error;
        }

        SocketOption option;
        option.reuseAddr = true;
        option.noDelay = true;
        error = listenSocket_.setOption(option);
        if (error.isFailure()) {
            listenSocket_.close();
            cleanupServerTls();
            running_.store(false, std::memory_order_release);
            return error;
        }

        error = listenSocket_.bind(bindAddress, port);
        if (error.isFailure()) {
            listenSocket_.close();
            cleanupServerTls();
            running_.store(false, std::memory_order_release);
            return error;
        }

        constexpr int listenBacklog = SOMAXCONN;
#ifdef __linux__
        configureLinuxListenSocket(listenSocket_.getFd(), listenBacklog);
#endif

        error = listenSocket_.listen(listenBacklog);
        if (error.isFailure()) {
            listenSocket_.close();
            cleanupServerTls();
            running_.store(false, std::memory_order_release);
            return error;
        }

        error = listenSocket_.setNonBlocking(true);
        if (error.isFailure()) {
            listenSocket_.close();
            cleanupServerTls();
            running_.store(false, std::memory_order_release);
            return error;
        }

        std::string actualListenIp;
        uint16_t actualListenPort = port;
        if (listenSocket_.getLocalAddress(actualListenIp, actualListenPort).isSuccess()) {
            listenAddress_ = Address(actualListenIp, actualListenPort);
        } else {
            listenAddress_ = Address(bindAddress, port);
        }

#ifdef _WIN32
        acceptEx_ = loadAcceptEx(listenSocket_.getFd());
        if (acceptEx_ == nullptr) {
            listenSocket_.close();
            cleanupServerTls();
            running_.store(false, std::memory_order_release);
            return FASTNET_ERROR(ErrorCode::SocketError, "Failed to load AcceptEx");
        }

        const Error acceptStartupError = startWindowsAcceptWorkers();
        if (acceptStartupError.isFailure()) {
            listenSocket_.close();
            cleanupServerTls();
            running_.store(false, std::memory_order_release);
            return acceptStartupError;
        }
#else
        if (!ioService_.getPoller().add(
                listenSocket_.getFd(),
                EventType::Read | EventType::Error | EventType::Close,
                [this](socket_t, EventType events, void*, size_t) {
                    if (hasEvent(events, EventType::Read)) {
                        acceptNewConnections();
                    }
                    if (hasEvent(events, EventType::Error) || hasEvent(events, EventType::Close)) {
                        reportError(FASTNET_ERROR(ErrorCode::SocketError, "Listen socket failure"));
                    }
                })) {
            listenSocket_.close();
            cleanupServerTls();
            running_.store(false, std::memory_order_release);
            return FASTNET_ERROR(ErrorCode::SocketError, "Failed to register listen socket");
        }
#endif

        return FASTNET_SUCCESS;
    }

    void stop() {
        if (!running_.exchange(false, std::memory_order_acq_rel)) {
            cleanupServerTls();
            return;
        }

#ifndef _WIN32
        ioService_.getPoller().remove(listenSocket_.getFd());
#endif
        listenSocket_.close();
#ifdef _WIN32
        shutdownWindowsAcceptWorkers();
#endif

        std::vector<std::shared_ptr<ClientSession>> sessions;
        {
            std::unique_lock<std::shared_mutex> lock(sessionsMutex_);
            for (auto& entry : sessions_) {
                sessions.push_back(entry.second);
            }
            sessions_.clear();
            sessionCount_.store(0, std::memory_order_release);
        }

        for (auto& session : sessions) {
            session->close("Server stopped");
        }

        cleanupServerTls();
    }

    Error sendToClient(ConnectionId clientId, const Buffer& data) {
        auto session = findSession(clientId);
        if (!session) {
            return FASTNET_ERROR(ErrorCode::InvalidArgument, "Client not found");
        }
        if (!session->send(data)) {
            return FASTNET_ERROR(ErrorCode::ConnectionError, "Client connection is not ready");
        }
        return FASTNET_SUCCESS;
    }

    Error sendToClient(ConnectionId clientId, Buffer&& data) {
        auto session = findSession(clientId);
        if (!session) {
            return FASTNET_ERROR(ErrorCode::InvalidArgument, "Client not found");
        }
        if (!session->send(std::move(data))) {
            return FASTNET_ERROR(ErrorCode::ConnectionError, "Client connection is not ready");
        }
        return FASTNET_SUCCESS;
    }

    Error sendToClient(ConnectionId clientId, const std::shared_ptr<const Buffer>& data) {
        auto session = findSession(clientId);
        if (!session) {
            return FASTNET_ERROR(ErrorCode::InvalidArgument, "Client not found");
        }
        if (!session->sendSharedBuffer(data)) {
            return FASTNET_ERROR(ErrorCode::ConnectionError, "Client connection is not ready");
        }
        return FASTNET_SUCCESS;
    }

    Error sendToClient(ConnectionId clientId, std::string&& data) {
        auto session = findSession(clientId);
        if (!session) {
            return FASTNET_ERROR(ErrorCode::InvalidArgument, "Client not found");
        }
        if (!session->send(std::move(data))) {
            return FASTNET_ERROR(ErrorCode::ConnectionError, "Client connection is not ready");
        }
        return FASTNET_SUCCESS;
    }

    Error sendToClient(ConnectionId clientId, std::string_view data) {
        auto session = findSession(clientId);
        if (!session) {
            return FASTNET_ERROR(ErrorCode::InvalidArgument, "Client not found");
        }
        if (!session->send(data)) {
            return FASTNET_ERROR(ErrorCode::ConnectionError, "Client connection is not ready");
        }
        return FASTNET_SUCCESS;
    }

    Error sendFileToClient(ConnectionId clientId,
                           const Buffer& prefix,
                           const std::string& filePath,
                           uint64_t offset,
                           uint64_t length) {
        auto session = findSession(clientId);
        if (!session) {
            return FASTNET_ERROR(ErrorCode::InvalidArgument, "Client not found");
        }
        return session->sendFile(prefix, filePath, offset, length);
    }

    Error sendFileToClient(ConnectionId clientId,
                           std::string&& prefix,
                           const std::string& filePath,
                           uint64_t offset,
                           uint64_t length) {
        auto session = findSession(clientId);
        if (!session) {
            return FASTNET_ERROR(ErrorCode::InvalidArgument, "Client not found");
        }
        return session->sendFile(std::move(prefix), filePath, offset, length);
    }

    Error sendFileToClient(ConnectionId clientId,
                           std::string_view prefix,
                           const std::string& filePath,
                           uint64_t offset,
                           uint64_t length) {
        auto session = findSession(clientId);
        if (!session) {
            return FASTNET_ERROR(ErrorCode::InvalidArgument, "Client not found");
        }
        return session->sendFile(prefix, filePath, offset, length);
    }

    void broadcast(const Buffer& data) {
        if (data.empty()) {
            return;
        }

        auto sharedBuffer = std::make_shared<Buffer>(data);
        std::vector<std::shared_ptr<ClientSession>> sessions;
        {
            std::shared_lock<std::shared_mutex> lock(sessionsMutex_);
            for (auto& entry : sessions_) {
                sessions.push_back(entry.second);
            }
        }

        for (auto& session : sessions) {
            session->sendSharedBuffer(sharedBuffer);
        }
    }

    void broadcast(Buffer&& data) {
        if (data.empty()) {
            return;
        }

        auto sharedBuffer = std::make_shared<Buffer>(std::move(data));
        std::vector<std::shared_ptr<ClientSession>> sessions;
        {
            std::shared_lock<std::shared_mutex> lock(sessionsMutex_);
            for (auto& entry : sessions_) {
                sessions.push_back(entry.second);
            }
        }

        for (auto& session : sessions) {
            session->sendSharedBuffer(sharedBuffer);
        }
    }

    void broadcast(std::string&& data) {
        if (data.empty()) {
            return;
        }

        auto sharedString = std::make_shared<std::string>(std::move(data));
        std::vector<std::shared_ptr<ClientSession>> sessions;
        {
            std::shared_lock<std::shared_mutex> lock(sessionsMutex_);
            for (auto& entry : sessions_) {
                sessions.push_back(entry.second);
            }
        }

        for (auto& session : sessions) {
            session->sendSharedString(sharedString);
        }
    }

    void broadcast(std::string_view data) {
        if (data.empty()) {
            return;
        }

        auto sharedString = std::make_shared<std::string>(data);
        std::vector<std::shared_ptr<ClientSession>> sessions;
        {
            std::shared_lock<std::shared_mutex> lock(sessionsMutex_);
            for (auto& entry : sessions_) {
                sessions.push_back(entry.second);
            }
        }

        for (auto& session : sessions) {
            session->sendSharedString(sharedString);
        }
    }

    void disconnectClient(ConnectionId clientId) {
        auto session = findSession(clientId);
        if (session) {
            session->close("Connection closed");
        }
    }

    void closeClientAfterPendingWrites(ConnectionId clientId) {
        auto session = findSession(clientId);
        if (session) {
            session->closeAfterPendingWrites();
        }
    }

    size_t getClientCount() const {
        return sessionCount_.load(std::memory_order_acquire);
    }

    std::vector<ConnectionId> getClientIds() const {
        std::shared_lock<std::shared_mutex> lock(sessionsMutex_);
        std::vector<ConnectionId> ids;
        ids.reserve(sessions_.size());
        for (const auto& entry : sessions_) {
            ids.push_back(entry.first);
        }
        return ids;
    }

    Address getClientAddress(ConnectionId clientId) const {
        std::shared_lock<std::shared_mutex> lock(sessionsMutex_);
        auto it = sessions_.find(clientId);
        if (it == sessions_.end()) {
            return {};
        }
        return it->second->address();
    }
    bool hasClient(ConnectionId clientId) const {
        std::shared_lock<std::shared_mutex> lock(sessionsMutex_);
        return sessions_.find(clientId) != sessions_.end();
    }

    void setCallbacks(const ServerClientConnectedCallback& connectedCb,
                      const ServerClientDisconnectedCallback& disconnectedCb,
                      const ServerDataReceivedCallback& dataReceivedCb,
                      const ServerDataReceivedOwnedCallback& ownedDataReceivedCb,
                      const ServerDataReceivedSharedCallback& sharedDataReceivedCb,
                      const ServerErrorCallback& errorCb) {
        std::unique_lock<std::shared_mutex> lock(callbackMutex_);
        connectedCallback_ = connectedCb;
        disconnectedCallback_ = disconnectedCb;
        dataReceivedCallback_ = dataReceivedCb;
        ownedDataReceivedCallback_ = ownedDataReceivedCb;
        sharedDataReceivedCallback_ = sharedDataReceivedCb;
        errorCallback_ = errorCb;
    }

    void setConnectionTimeout(uint32_t timeoutMs) {
        connectionTimeout_ = timeoutMs;
        applyTimeoutsToSessions();
    }
    void setReadTimeout(uint32_t timeoutMs) {
        readTimeout_ = timeoutMs;
        applyTimeoutsToSessions();
    }
    void setWriteTimeout(uint32_t timeoutMs) {
        writeTimeout_ = timeoutMs;
        applyTimeoutsToSessions();
    }
    void setMaxConnections(size_t maxConnections) { maxConnections_ = maxConnections; }
    Address getListenAddress() const { return listenAddress_; }
    bool isRunning() const { return running_.load(std::memory_order_acquire); }

private:
    std::shared_ptr<ClientSession> findSession(ConnectionId clientId) const {
        std::shared_lock<std::shared_mutex> lock(sessionsMutex_);
        auto it = sessions_.find(clientId);
        return it == sessions_.end() ? nullptr : it->second;
    }

    void assignSessionCallbacks(const std::shared_ptr<ClientSession>& session) {
        if (!session) {
            return;
        }

        bool hasDataCallback = false;
        bool hasOwnedDataCallback = false;
        bool hasSharedDataCallback = false;
        bool hasErrorCallback = false;
        {
            std::shared_lock<std::shared_mutex> lock(callbackMutex_);
            hasDataCallback = static_cast<bool>(dataReceivedCallback_);
            hasOwnedDataCallback = static_cast<bool>(ownedDataReceivedCallback_);
            hasSharedDataCallback = static_cast<bool>(sharedDataReceivedCallback_);
            hasErrorCallback = static_cast<bool>(errorCallback_);
        }

        ServerDataReceivedCallback dataCallback;
        if (hasDataCallback) {
            dataCallback = [this](ConnectionId id, const Buffer& data) {
                dispatchDataReceived(id, data);
            };
        }

        ServerDataReceivedOwnedCallback ownedCallback;
        if (hasOwnedDataCallback) {
            ownedCallback = [this](ConnectionId id, Buffer&& data) {
                dispatchOwnedDataReceived(id, std::move(data));
            };
        }

        ServerDataReceivedSharedCallback sharedCallback;
        if (hasSharedDataCallback) {
            sharedCallback = [this](ConnectionId id, const std::shared_ptr<const Buffer>& data) {
                dispatchSharedDataReceived(id, data);
            };
        }

        ServerErrorCallback sessionErrorCallback;
        if (hasErrorCallback) {
            sessionErrorCallback = [this](const Error& error) {
                reportError(error);
            };
        }

        session->setCallbacks(
            [this](ConnectionId id, const Address& address) {
                onSessionReady(id, address);
            },
            dataCallback,
            ownedCallback,
            sharedCallback,
            [this](ConnectionId id, bool wasReady, const std::string& reason) {
                onSessionClosed(id, wasReady, reason);
            },
            sessionErrorCallback);
    }

#ifdef _WIN32
    Error submitAccept(AcceptContext& context) {
        closeSocketHandle(context.acceptedFd);
        context.overlapped = OVERLAPPED{};
        context.addressBuffer.fill(0);

        const int family = listenSocket_.getFamily() == AF_UNSPEC ? AF_INET : listenSocket_.getFamily();
        context.acceptedFd = createOverlappedAcceptSocket(family);
        if (context.acceptedFd == INVALID_SOCKET_FD) {
            return Error(ErrorCode::SocketError,
                         "Failed to create AcceptEx socket",
                         SocketWrapper::getLastSocketError());
        }

        DWORD bytesReceived = 0;
        const BOOL started = acceptEx_(listenSocket_.getFd(),
                                       context.acceptedFd,
                                       context.addressBuffer.data(),
                                       0,
                                       kAcceptExAddressLength,
                                       kAcceptExAddressLength,
                                       &bytesReceived,
                                       &context.overlapped);
        if (started == FALSE) {
            const int error = ::WSAGetLastError();
            if (error != ERROR_IO_PENDING) {
                closeSocketHandle(context.acceptedFd);
                return Error(ErrorCode::SocketError, "AcceptEx failed", error);
            }
        }

        return FASTNET_SUCCESS;
    }

    Error startWindowsAcceptWorkers() {
        acceptCompletionPort_ = createCompletionPort();
        if (acceptCompletionPort_ == nullptr) {
            return Error(ErrorCode::SocketError,
                         "Failed to create accept completion port",
                         static_cast<int>(::GetLastError()));
        }

        if (!associateSocketWithCompletionPort(acceptCompletionPort_, listenSocket_.getFd())) {
            const int error = static_cast<int>(::GetLastError());
            closeCompletionPort(acceptCompletionPort_);
            return Error(ErrorCode::SocketError, "Failed to associate listen socket with completion port", error);
        }

        Error startupError = FASTNET_SUCCESS;
        const size_t acceptWorkerCount = determineAcceptWorkerCount();
        const size_t pendingAcceptCount = determinePendingAcceptCount();
        try {
            acceptContexts_.reserve(pendingAcceptCount);
            acceptThreads_.reserve(acceptWorkerCount);

            for (size_t i = 0; i < pendingAcceptCount; ++i) {
                auto context = std::make_unique<AcceptContext>();
                const Error submitError = submitAccept(*context);
                if (submitError.isFailure()) {
                    if (acceptContexts_.empty()) {
                        startupError = submitError;
                    }
                    break;
                }
                acceptContexts_.push_back(std::move(context));
            }

            if (startupError.isSuccess()) {
                for (size_t i = 0; i < acceptWorkerCount; ++i) {
                    acceptThreads_.emplace_back([this]() {
                        acceptLoop();
                    });
                }
            }
        } catch (...) {
            if (startupError.isSuccess()) {
                startupError = FASTNET_ERROR(ErrorCode::SocketError, "Failed to start accept worker threads");
            }
        }

        if (startupError.isFailure()) {
            shutdownWindowsAcceptWorkers();
        }
        return startupError;
    }

    void shutdownWindowsAcceptWorkers() {
        if (acceptCompletionPort_ != nullptr) {
            for (size_t i = 0; i < acceptThreads_.size(); ++i) {
                ::PostQueuedCompletionStatus(acceptCompletionPort_, 0, kAcceptShutdownCompletionKey, nullptr);
            }
        }

        for (auto& thread : acceptThreads_) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        acceptThreads_.clear();
        acceptContexts_.clear();
        closeCompletionPort(acceptCompletionPort_);
        acceptEx_ = nullptr;
    }

    void acceptLoop() {
        while (true) {
            DWORD transferred = 0;
            ULONG_PTR completionKey = 0;
            LPOVERLAPPED overlapped = nullptr;
            const BOOL completed = ::GetQueuedCompletionStatus(
                acceptCompletionPort_, &transferred, &completionKey, &overlapped, INFINITE);
            (void)transferred;

            if (completionKey == kAcceptShutdownCompletionKey && overlapped == nullptr) {
                break;
            }

            if (overlapped == nullptr) {
                if (!completed && running_.load(std::memory_order_acquire)) {
                    reportError(
                        Error(ErrorCode::SocketError,
                              "Accept completion wait failed",
                              static_cast<int>(::GetLastError())));
                }
                continue;
            }

            auto* context = reinterpret_cast<AcceptContext*>(overlapped);
            const int completionError = completed ? 0 : static_cast<int>(::GetLastError());
            if (completionError == 0) {
                if (running_.load(std::memory_order_acquire)) {
                    socket_t listenFd = listenSocket_.getFd();
                    if (::setsockopt(context->acceptedFd,
                                     SOL_SOCKET,
                                     SO_UPDATE_ACCEPT_CONTEXT,
                                     reinterpret_cast<const char*>(&listenFd),
                                     static_cast<int>(sizeof(listenFd))) != 0) {
                        const int updateError = ::WSAGetLastError();
                        if (running_.load(std::memory_order_acquire) || !isExpectedAcceptShutdownError(updateError)) {
                            reportError(Error(ErrorCode::SocketError, "Failed to update accept context", updateError));
                        }
                    } else {
                        std::string clientIp;
                        uint16_t clientPort = 0;
                        if (extractAcceptedRemoteEndpoint(context->addressBuffer, clientIp, clientPort) ||
                            queryPeerEndpoint(context->acceptedFd, clientIp, clientPort)) {
                            socket_t acceptedFd = context->acceptedFd;
                            context->acceptedFd = INVALID_SOCKET_FD;
                            adoptAcceptedSocket(acceptedFd, clientIp, clientPort);
                        } else {
                            reportError(FASTNET_ERROR(ErrorCode::SocketError,
                                                      "Failed to format accepted peer address"));
                        }
                    }
                }
            } else if (running_.load(std::memory_order_acquire) || !isExpectedAcceptShutdownError(completionError)) {
                reportError(Error(ErrorCode::SocketError, "AcceptEx completion failed", completionError));
            }

            while (running_.load(std::memory_order_acquire)) {
                const Error submitError = submitAccept(*context);
                if (submitError.isSuccess()) {
                    break;
                }

                const int systemCode = submitError.getSystemCode();
                if (!running_.load(std::memory_order_acquire) && isExpectedAcceptShutdownError(systemCode)) {
                    break;
                }

                reportError(submitError);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    }
#endif

    void acceptNewConnections() {
        while (running_.load(std::memory_order_acquire)) {
            std::string clientIp;
            uint16_t clientPort = 0;
            socket_t clientFd = INVALID_SOCKET_FD;
            Error error = listenSocket_.accept(clientFd, clientIp, clientPort);
            if (error.isFailure()) {
                const int code = SocketWrapper::getLastSocketError();
                if (isSocketWouldBlock(code)) {
                    return;
                }
                reportError(error);
                return;
            }

            adoptAcceptedSocket(clientFd, clientIp, clientPort);
        }
    }

    void adoptAcceptedSocket(socket_t clientFd, const std::string& clientIp, uint16_t clientPort) {
        if (sessionCount_.load(std::memory_order_acquire) >= maxConnections_) {
            closesocket(clientFd);
            return;
        }

        const ConnectionId connectionId = nextConnectionId_.fetch_add(1, std::memory_order_relaxed);
        std::shared_ptr<ClientSession> session;
        try {
            Address clientAddress(clientIp, clientPort);
            session = std::make_shared<ClientSession>(
                ioService_,
                clientFd,
                connectionId,
                clientAddress,
                sslEnabled_ ? &sslContext_ : nullptr,
                currentTimeoutConfig());

            assignSessionCallbacks(session);
        } catch (const std::exception& ex) {
            if (!session) {
                closesocket(clientFd);
            }
            reportError(Error(ErrorCode::UnknownError,
                              std::string("Failed to create server session: ") + ex.what()));
            return;
        } catch (...) {
            if (!session) {
                closesocket(clientFd);
            }
            reportError(Error(ErrorCode::UnknownError, "Failed to create server session"));
            return;
        }

        {
            std::unique_lock<std::shared_mutex> lock(sessionsMutex_);
            if (sessions_.size() >= maxConnections_) {
                return;
            }
            sessions_[connectionId] = session;
            sessionCount_.fetch_add(1, std::memory_order_release);
        }

        if (!session->start()) {
            {
                std::unique_lock<std::shared_mutex> lock(sessionsMutex_);
                if (sessions_.erase(connectionId) > 0) {
                    sessionCount_.fetch_sub(1, std::memory_order_release);
                }
            }
            reportError(Error(session->getLastFailureCode(), session->getLastFailureMessage()));
        }
    }

    void onSessionReady(ConnectionId connectionId, const Address& clientAddress) {
        ServerClientConnectedCallback callback;
        {
            std::shared_lock<std::shared_mutex> lock(callbackMutex_);
            callback = connectedCallback_;
        }
        if (callback) {
            callback(connectionId, clientAddress);
        }
    }

    void onSessionClosed(ConnectionId connectionId, bool wasReady, const std::string& reason) {
        {
            std::unique_lock<std::shared_mutex> lock(sessionsMutex_);
            if (sessions_.erase(connectionId) > 0) {
                sessionCount_.fetch_sub(1, std::memory_order_release);
            }
        }

        ServerClientDisconnectedCallback callback;
        {
            std::shared_lock<std::shared_mutex> lock(callbackMutex_);
            callback = disconnectedCallback_;
        }
        if (wasReady && callback) {
            callback(connectionId, reason);
        }
    }

    void reportError(const Error& error) const {
        ServerErrorCallback callback;
        {
            std::shared_lock<std::shared_mutex> lock(callbackMutex_);
            callback = errorCallback_;
        }
        if (callback) {
            callback(error);
        }
    }

    void dispatchDataReceived(ConnectionId connectionId, const Buffer& data) const {
        ServerDataReceivedCallback callback;
        {
            std::shared_lock<std::shared_mutex> lock(callbackMutex_);
            callback = dataReceivedCallback_;
        }
        if (callback) {
            callback(connectionId, data);
        }
    }

    void dispatchOwnedDataReceived(ConnectionId connectionId, Buffer&& data) const {
        ServerDataReceivedOwnedCallback callback;
        {
            std::shared_lock<std::shared_mutex> lock(callbackMutex_);
            callback = ownedDataReceivedCallback_;
        }
        if (callback) {
            callback(connectionId, std::move(data));
        }
    }

    void dispatchSharedDataReceived(ConnectionId connectionId, const std::shared_ptr<const Buffer>& data) const {
        ServerDataReceivedSharedCallback callback;
        {
            std::shared_lock<std::shared_mutex> lock(callbackMutex_);
            callback = sharedDataReceivedCallback_;
        }
        if (callback) {
            callback(connectionId, data);
        }
    }

    SessionTimeoutConfig currentTimeoutConfig() const {
        SessionTimeoutConfig config;
        config.connectionTimeoutMs = connectionTimeout_;
        config.readTimeoutMs = readTimeout_;
        config.writeTimeoutMs = writeTimeout_;
        return config;
    }

    void applyTimeoutsToSessions() {
        std::vector<std::shared_ptr<ClientSession>> sessions;
        {
            std::shared_lock<std::shared_mutex> lock(sessionsMutex_);
            for (const auto& entry : sessions_) {
                sessions.push_back(entry.second);
            }
        }

        const SessionTimeoutConfig config = currentTimeoutConfig();
        for (const auto& session : sessions) {
            session->updateTimeouts(config);
        }
    }

    void cleanupServerTls() {
        sslEnabled_ = false;
        sslContext_.cleanup();
    }

#ifdef _WIN32
    size_t determineAcceptWorkerCount() const {
        const size_t ioThreadCount = std::max<size_t>(ioService_.getThreadCount(), 1);
        return std::min<size_t>((std::max)(ioThreadCount * 2, size_t(2)), kMaxWindowsAcceptWorkers);
    }

    size_t determinePendingAcceptCount() const {
        const size_t connectionBound = (std::max)(size_t(1), maxConnections_);
        return (std::min)(connectionBound, kMaxWindowsPendingAccepts);
    }
#endif

    IoService& ioService_;
    SocketWrapper listenSocket_;
    std::atomic<bool> running_{false};
    mutable std::shared_mutex sessionsMutex_;
    std::unordered_map<ConnectionId, std::shared_ptr<ClientSession>> sessions_;
    std::atomic<size_t> sessionCount_{0};
    Address listenAddress_;
    size_t maxConnections_ = 10000;
    uint32_t connectionTimeout_ = 0;
    uint32_t readTimeout_ = 0;
    uint32_t writeTimeout_ = 0;
    std::atomic<ConnectionId> nextConnectionId_{1};
    SSLConfig sslConfig_;
    bool sslEnabled_ = false;
    SSLContext sslContext_;
#ifdef _WIN32
    std::vector<std::thread> acceptThreads_;
    std::vector<std::unique_ptr<AcceptContext>> acceptContexts_;
    HANDLE acceptCompletionPort_ = nullptr;
    LPFN_ACCEPTEX acceptEx_ = nullptr;
#endif

    mutable std::shared_mutex callbackMutex_;
    ServerClientConnectedCallback connectedCallback_;
    ServerClientDisconnectedCallback disconnectedCallback_;
    ServerDataReceivedCallback dataReceivedCallback_;
    ServerDataReceivedOwnedCallback ownedDataReceivedCallback_;
    ServerDataReceivedSharedCallback sharedDataReceivedCallback_;
    ServerErrorCallback errorCallback_;
};

TcpServer::TcpServer(IoService& ioService)
    : impl_(std::make_unique<Impl>(ioService)) {}

TcpServer::~TcpServer() = default;

Error TcpServer::start(uint16_t port, const std::string& bindAddress, const SSLConfig& sslConfig) {
    return impl_->start(port, bindAddress, sslConfig);
}

Error TcpServer::start(const Address& listenAddress, const SSLConfig& sslConfig) {
    return impl_->start(listenAddress.port, listenAddress.host(), sslConfig);
}

void TcpServer::stop() {
    impl_->stop();
}

Error TcpServer::sendToClient(ConnectionId clientId, const Buffer& data) {
    return impl_->sendToClient(clientId, data);
}

Error TcpServer::sendToClient(ConnectionId clientId, Buffer&& data) {
    return impl_->sendToClient(clientId, std::move(data));
}

Error TcpServer::sendToClient(ConnectionId clientId, const std::shared_ptr<const Buffer>& data) {
    return impl_->sendToClient(clientId, data);
}

Error TcpServer::sendToClient(ConnectionId clientId, std::string&& data) {
    return impl_->sendToClient(clientId, std::move(data));
}

Error TcpServer::sendToClient(ConnectionId clientId, std::string_view data) {
    return impl_->sendToClient(clientId, data);
}

Error TcpServer::sendFileToClient(ConnectionId clientId,
                                  const Buffer& prefix,
                                  const std::string& filePath,
                                  uint64_t offset,
                                  uint64_t length) {
    return impl_->sendFileToClient(clientId, prefix, filePath, offset, length);
}

Error TcpServer::sendFileToClient(ConnectionId clientId,
                                  std::string&& prefix,
                                  const std::string& filePath,
                                  uint64_t offset,
                                  uint64_t length) {
    return impl_->sendFileToClient(clientId, std::move(prefix), filePath, offset, length);
}

Error TcpServer::sendFileToClient(ConnectionId clientId,
                                  std::string_view prefix,
                                  const std::string& filePath,
                                  uint64_t offset,
                                  uint64_t length) {
    return impl_->sendFileToClient(clientId, prefix, filePath, offset, length);
}

void TcpServer::broadcast(const Buffer& data) {
    impl_->broadcast(data);
}

void TcpServer::broadcast(Buffer&& data) {
    impl_->broadcast(std::move(data));
}

void TcpServer::broadcast(std::string&& data) {
    impl_->broadcast(std::move(data));
}

void TcpServer::broadcast(std::string_view data) {
    impl_->broadcast(data);
}

void TcpServer::disconnectClient(ConnectionId clientId) {
    impl_->disconnectClient(clientId);
}

void TcpServer::closeClientAfterPendingWrites(ConnectionId clientId) {
    impl_->closeClientAfterPendingWrites(clientId);
}

size_t TcpServer::getClientCount() const {
    return impl_->getClientCount();
}

std::vector<ConnectionId> TcpServer::getClientIds() const {
    return impl_->getClientIds();
}

Address TcpServer::getClientAddress(ConnectionId clientId) const {
    return impl_->getClientAddress(clientId);
}

bool TcpServer::hasClient(ConnectionId clientId) const {
    return impl_->hasClient(clientId);
}

void TcpServer::setClientConnectedCallback(const ServerClientConnectedCallback& callback) {
    clientConnectedCallback_ = callback;
    impl_->setCallbacks(clientConnectedCallback_,
                        clientDisconnectedCallback_,
                        dataReceivedCallback_,
                        ownedDataReceivedCallback_,
                        sharedDataReceivedSharedCallback_,
                        serverErrorCallback_);
}

void TcpServer::setClientDisconnectedCallback(const ServerClientDisconnectedCallback& callback) {
    clientDisconnectedCallback_ = callback;
    impl_->setCallbacks(clientConnectedCallback_,
                        clientDisconnectedCallback_,
                        dataReceivedCallback_,
                        ownedDataReceivedCallback_,
                        sharedDataReceivedSharedCallback_,
                        serverErrorCallback_);
}

void TcpServer::setDataReceivedCallback(const ServerDataReceivedCallback& callback) {
    dataReceivedCallback_ = callback;
    ownedDataReceivedCallback_ = nullptr;
    sharedDataReceivedSharedCallback_ = nullptr;
    impl_->setCallbacks(clientConnectedCallback_,
                        clientDisconnectedCallback_,
                        dataReceivedCallback_,
                        ownedDataReceivedCallback_,
                        sharedDataReceivedSharedCallback_,
                        serverErrorCallback_);
}

void TcpServer::setOwnedDataReceivedCallback(const ServerDataReceivedOwnedCallback& callback) {
    ownedDataReceivedCallback_ = callback;
    dataReceivedCallback_ = nullptr;
    sharedDataReceivedSharedCallback_ = nullptr;
    impl_->setCallbacks(clientConnectedCallback_,
                        clientDisconnectedCallback_,
                        dataReceivedCallback_,
                        ownedDataReceivedCallback_,
                        sharedDataReceivedSharedCallback_,
                        serverErrorCallback_);
}

void TcpServer::setSharedDataReceivedCallback(const ServerDataReceivedSharedCallback& callback) {
    sharedDataReceivedSharedCallback_ = callback;
    dataReceivedCallback_ = nullptr;
    ownedDataReceivedCallback_ = nullptr;
    impl_->setCallbacks(clientConnectedCallback_,
                        clientDisconnectedCallback_,
                        dataReceivedCallback_,
                        ownedDataReceivedCallback_,
                        sharedDataReceivedSharedCallback_,
                        serverErrorCallback_);
}

void TcpServer::setServerErrorCallback(const ServerErrorCallback& callback) {
    serverErrorCallback_ = callback;
    impl_->setCallbacks(clientConnectedCallback_,
                        clientDisconnectedCallback_,
                        dataReceivedCallback_,
                        ownedDataReceivedCallback_,
                        sharedDataReceivedSharedCallback_,
                        serverErrorCallback_);
}

void TcpServer::setConnectionTimeout(uint32_t timeoutMs) {
    impl_->setConnectionTimeout(timeoutMs);
}

void TcpServer::setReadTimeout(uint32_t timeoutMs) {
    impl_->setReadTimeout(timeoutMs);
}

void TcpServer::setWriteTimeout(uint32_t timeoutMs) {
    impl_->setWriteTimeout(timeoutMs);
}

void TcpServer::setMaxConnections(size_t maxConnections) {
    impl_->setMaxConnections(maxConnections);
}

Address TcpServer::getListenAddress() const {
    return impl_->getListenAddress();
}

bool TcpServer::isRunning() const {
    return impl_->isRunning();
}

} // namespace FastNet
