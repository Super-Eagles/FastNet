/**
 * @file SocketWrapper.cpp
 * @brief FastNet native socket abstraction
 */
#include "SocketWrapper.h"

#ifdef Error
#undef Error
#endif

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#ifdef _WIN32
#include <fileapi.h>
#include <mswsock.h>
#include <mstcpip.h>
#include <winbase.h>
#else
#include <csignal>
#include <netdb.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#endif

namespace FastNet {
namespace {

struct ResolvedAddress {
    sockaddr_storage storage{};
    socket_len_t length = 0;
    int family = AF_UNSPEC;
};

int nativeSocketType(SocketType type) noexcept {
    return type == SocketType::TCP ? SOCK_STREAM : SOCK_DGRAM;
}

int nativeProtocol(SocketType type) noexcept {
    return type == SocketType::TCP ? IPPROTO_TCP : IPPROTO_UDP;
}

bool isConnectInProgressError(int errorCode) noexcept {
#ifdef _WIN32
    return errorCode == WSAEWOULDBLOCK || errorCode == WSAEINPROGRESS || errorCode == WSAEALREADY;
#else
    return errorCode == EINPROGRESS || errorCode == EALREADY;
#endif
}

std::string normalizeHost(std::string_view host) {
    return std::string(Address::stripBrackets(host));
}

bool copyResolvedAddress(const sockaddr* source, socket_len_t length, ResolvedAddress& target) {
    if (source == nullptr || length <= 0 || static_cast<size_t>(length) > sizeof(sockaddr_storage)) {
        return false;
    }
    std::memset(&target.storage, 0, sizeof(target.storage));
    std::memcpy(&target.storage, source, static_cast<size_t>(length));
    target.length = length;
    target.family = source->sa_family;
    return true;
}

bool tryBuildNumericAddress(std::string_view host, uint16_t port, ResolvedAddress& result) {
    const std::string normalized = normalizeHost(host);
    if (normalized.empty()) {
        return false;
    }

    if (normalized == "0.0.0.0" || Address::isValidIPv4(normalized)) {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        if (normalized == "0.0.0.0") {
            address.sin_addr.s_addr = htonl(INADDR_ANY);
        } else if (::inet_pton(AF_INET, normalized.c_str(), &address.sin_addr) != 1) {
            return false;
        }
        return copyResolvedAddress(reinterpret_cast<const sockaddr*>(&address),
                                   static_cast<socket_len_t>(sizeof(address)),
                                   result);
    }

    if (normalized == "::" || Address::isValidIPv6(normalized)) {
        sockaddr_in6 address{};
        address.sin6_family = AF_INET6;
        address.sin6_port = htons(port);
        if (normalized == "::") {
            address.sin6_addr = in6addr_any;
        } else if (::inet_pton(AF_INET6, normalized.c_str(), &address.sin6_addr) != 1) {
            return false;
        }
        return copyResolvedAddress(reinterpret_cast<const sockaddr*>(&address),
                                   static_cast<socket_len_t>(sizeof(address)),
                                   result);
    }

    return false;
}

Result<std::vector<ResolvedAddress>> resolveAddresses(std::string_view host,
                                                      uint16_t port,
                                                      SocketType type,
                                                      bool passive,
                                                      int familyHint) {
    if (!host.empty()) {
        ResolvedAddress numericAddress;
        if (tryBuildNumericAddress(host, port, numericAddress)) {
            std::vector<ResolvedAddress> addresses;
            addresses.push_back(numericAddress);
            return Result<std::vector<ResolvedAddress>>::success(std::move(addresses));
        }
    }

    addrinfo hints{};
    hints.ai_family = familyHint;
    hints.ai_socktype = nativeSocketType(type);
    hints.ai_protocol = nativeProtocol(type);
    hints.ai_flags = AI_NUMERICSERV;
    if (passive) {
        hints.ai_flags |= AI_PASSIVE;
    }

    const std::string normalizedHost = normalizeHost(host);
    const bool useNullHost = passive && (normalizedHost.empty() || Address::isAnyHost(normalizedHost));
    const char* hostPtr = useNullHost ? nullptr : normalizedHost.c_str();
    const std::string service = std::to_string(port);

    addrinfo* rawResults = nullptr;
    const int resolveResult = ::getaddrinfo(hostPtr, service.c_str(), &hints, &rawResults);
    if (resolveResult != 0) {
#ifdef _WIN32
        return Result<std::vector<ResolvedAddress>>::error(
            Error(ErrorCode::ResolveError, ::gai_strerrorA(resolveResult), resolveResult));
#else
        return Result<std::vector<ResolvedAddress>>::error(
            Error(ErrorCode::ResolveError, ::gai_strerror(resolveResult), resolveResult));
#endif
    }

    std::vector<ResolvedAddress> addresses;
    for (addrinfo* current = rawResults; current != nullptr; current = current->ai_next) {
        ResolvedAddress entry;
        if (copyResolvedAddress(current->ai_addr, static_cast<socket_len_t>(current->ai_addrlen), entry)) {
            addresses.push_back(entry);
        }
    }
    ::freeaddrinfo(rawResults);

    if (addresses.empty()) {
        return Result<std::vector<ResolvedAddress>>::error(
            Error(ErrorCode::ResolveError, "Address resolution returned no usable endpoints"));
    }
    return Result<std::vector<ResolvedAddress>>::success(std::move(addresses));
}

Error endpointToText(const sockaddr* address, std::string& ip, uint16_t& port) {
    if (address == nullptr) {
        return FASTNET_ERROR(ErrorCode::InvalidArgument, "Invalid socket address");
    }

    char ipBuffer[INET6_ADDRSTRLEN] = {0};
    if (address->sa_family == AF_INET) {
        const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(address);
        if (::inet_ntop(AF_INET, &ipv4->sin_addr, ipBuffer, static_cast<socklen_t>(sizeof(ipBuffer))) == nullptr) {
            return FASTNET_SYSTEM_ERROR(ErrorCode::SocketError, "Failed to format IPv4 address");
        }
        ip = ipBuffer;
        port = ntohs(ipv4->sin_port);
        return FASTNET_SUCCESS;
    }

    if (address->sa_family == AF_INET6) {
        const auto* ipv6 = reinterpret_cast<const sockaddr_in6*>(address);
        if (::inet_ntop(AF_INET6, &ipv6->sin6_addr, ipBuffer, static_cast<socklen_t>(sizeof(ipBuffer))) == nullptr) {
            return FASTNET_SYSTEM_ERROR(ErrorCode::SocketError, "Failed to format IPv6 address");
        }
        ip = ipBuffer;
        port = ntohs(ipv6->sin6_port);
        return FASTNET_SUCCESS;
    }

    return FASTNET_ERROR(ErrorCode::SocketError, "Unsupported socket address family");
}

bool isLengthRepresentable(size_t length) noexcept {
    return length <= static_cast<size_t>((std::numeric_limits<int>::max)());
}

} // namespace

#ifdef _WIN32
static std::atomic<int> g_wsaInitCount{0};
#endif

SocketWrapper::SocketWrapper() noexcept = default;

SocketWrapper::SocketWrapper(socket_t fd) noexcept
    : fd_(fd) {
    family_ = detectFamily();
}

SocketWrapper::~SocketWrapper() {
    close();
}

SocketWrapper::SocketWrapper(SocketWrapper&& other) noexcept
    : fd_(other.fd_),
      family_(other.family_),
      type_(other.type_),
      nonBlocking_(other.nonBlocking_),
      lastError_(std::move(other.lastError_)) {
    other.fd_ = INVALID_SOCKET_FD;
    other.family_ = AF_UNSPEC;
    other.nonBlocking_ = false;
}

SocketWrapper& SocketWrapper::operator=(SocketWrapper&& other) noexcept {
    if (this == &other) {
        return *this;
    }

    close();
    fd_ = other.fd_;
    family_ = other.family_;
    type_ = other.type_;
    nonBlocking_ = other.nonBlocking_;
    lastError_ = std::move(other.lastError_);

    other.fd_ = INVALID_SOCKET_FD;
    other.family_ = AF_UNSPEC;
    other.nonBlocking_ = false;
    return *this;
}

Error SocketWrapper::create(SocketType type) {
    type_ = type;
    return createNativeSocket(AF_INET);
}

Error SocketWrapper::create(SocketType type, const Address& address, bool passive) {
    return create(type, address.host(), address.port, passive);
}

Error SocketWrapper::create(SocketType type, std::string_view host, uint16_t port, bool passive) {
    type_ = type;
    const auto resolvedAddresses = resolveAddresses(host, port, type, passive, AF_UNSPEC);
    if (!resolvedAddresses) {
        lastError_ = resolvedAddresses.error();
        return lastError_;
    }
    return createNativeSocket(resolvedAddresses.value().front().family);
}

Error SocketWrapper::createNativeSocket(int family) {
    if (family == AF_UNSPEC) {
        family = AF_INET;
    }

    close();

    const int socketType = nativeSocketType(type_);
    const int protocol = nativeProtocol(type_);
#ifndef _WIN32
#ifdef SOCK_CLOEXEC
    fd_ = ::socket(family, socketType | SOCK_CLOEXEC, protocol);
    if (fd_ == INVALID_SOCKET_FD) {
        fd_ = ::socket(family, socketType, protocol);
    }
#else
    fd_ = ::socket(family, socketType, protocol);
#endif
#else
    fd_ = ::WSASocketW(family, socketType, protocol, nullptr, 0, WSA_FLAG_OVERLAPPED);
#endif

    if (fd_ == INVALID_SOCKET_FD) {
        lastError_ = Error(ErrorCode::SocketError, "Failed to create socket", getLastSocketError());
        family_ = AF_UNSPEC;
        return lastError_;
    }

    family_ = family;
    nonBlocking_ = false;

#if defined(IPPROTO_IPV6) && defined(IPV6_V6ONLY)
    if (family_ == AF_INET6) {
        int dualStack = 0;
        ::setsockopt(fd_,
                     IPPROTO_IPV6,
                     IPV6_V6ONLY,
                     reinterpret_cast<const char*>(&dualStack),
                     static_cast<socket_len_t>(sizeof(dualStack)));
    }
#endif

    lastError_ = FASTNET_SUCCESS;
    return lastError_;
}

Error SocketWrapper::bind(const std::string& ip, uint16_t port) {
    return bind(Address(ip, port));
}

Error SocketWrapper::bind(const Address& address) {
    if (!isValid()) {
        const Error createResult = create(type_, address, true);
        if (createResult.isFailure()) {
            return createResult;
        }
    }

    const auto resolvedAddresses = resolveAddresses(address.host(), address.port, type_, true, family_);
    if (!resolvedAddresses) {
        lastError_ = resolvedAddresses.error();
        return lastError_;
    }

    int lastSystemError = 0;
    for (const auto& candidate : resolvedAddresses.value()) {
        if (family_ != AF_UNSPEC && candidate.family != family_) {
            continue;
        }
        if (::bind(fd_,
                   reinterpret_cast<const sockaddr*>(&candidate.storage),
                   candidate.length) == 0) {
            lastError_ = FASTNET_SUCCESS;
            return lastError_;
        }
        lastSystemError = getLastSocketError();
    }

    lastError_ = lastSystemError != 0
                     ? Error(ErrorCode::BindError, "Failed to bind socket", lastSystemError)
                     : Error(ErrorCode::BindError, "No resolved endpoint matched the socket family");
    return lastError_;
}

Error SocketWrapper::listen(int backlog) {
    if (!isValid()) {
        lastError_ = FASTNET_ERROR(ErrorCode::InvalidArgument, "Invalid socket");
        return lastError_;
    }
    if (::listen(fd_, backlog) != 0) {
        lastError_ = Error(ErrorCode::ListenError, "Failed to listen on socket", getLastSocketError());
        return lastError_;
    }
    lastError_ = FASTNET_SUCCESS;
    return lastError_;
}

Error SocketWrapper::accept(socket_t& clientFd, std::string& clientIp, uint16_t& clientPort) {
    if (!isValid()) {
        lastError_ = FASTNET_ERROR(ErrorCode::InvalidArgument, "Invalid socket");
        return lastError_;
    }

    sockaddr_storage clientAddress{};
    socket_len_t clientAddressLength = static_cast<socket_len_t>(sizeof(clientAddress));

#ifdef __linux__
    socket_t acceptedFd = ::accept4(fd_,
                                    reinterpret_cast<sockaddr*>(&clientAddress),
                                    &clientAddressLength,
                                    SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (acceptedFd == INVALID_SOCKET_FD && errno == ENOSYS) {
        acceptedFd = ::accept(fd_,
                              reinterpret_cast<sockaddr*>(&clientAddress),
                              &clientAddressLength);
    }
#else
    socket_t acceptedFd = ::accept(fd_,
                                   reinterpret_cast<sockaddr*>(&clientAddress),
                                   &clientAddressLength);
#endif

    if (acceptedFd == INVALID_SOCKET_FD) {
        lastError_ = Error(ErrorCode::SocketError, "Accept failed", getLastSocketError());
        return lastError_;
    }

    const Error endpointError =
        endpointToText(reinterpret_cast<const sockaddr*>(&clientAddress), clientIp, clientPort);
    if (endpointError.isFailure()) {
        ::closesocket(acceptedFd);
        lastError_ = endpointError;
        return lastError_;
    }

    clientFd = acceptedFd;
    lastError_ = FASTNET_SUCCESS;
    return lastError_;
}

Error SocketWrapper::connect(const std::string& ip, uint16_t port, int timeoutMs) {
    return connect(Address(ip, port), timeoutMs);
}

Error SocketWrapper::connect(const Address& address, int timeoutMs) {
    if (!isValid()) {
        const Error createResult = create(type_, address, false);
        if (createResult.isFailure()) {
            return createResult;
        }
    }

    const auto resolvedAddresses = resolveAddresses(address.host(), address.port, type_, false, family_);
    if (!resolvedAddresses) {
        lastError_ = resolvedAddresses.error();
        return lastError_;
    }

    const ResolvedAddress* target = nullptr;
    for (const auto& candidate : resolvedAddresses.value()) {
        if (family_ == AF_UNSPEC || candidate.family == family_) {
            target = &candidate;
            break;
        }
    }

    if (target == nullptr) {
        lastError_ = Error(ErrorCode::ResolveError, "No resolved endpoint matched the socket family");
        return lastError_;
    }

    auto performConnect = [this, target]() -> Error {
        if (::connect(fd_,
                      reinterpret_cast<const sockaddr*>(&target->storage),
                      target->length) == 0) {
            return FASTNET_SUCCESS;
        }
        return Error(ErrorCode::ConnectionError, "Failed to connect to server", getLastSocketError());
    };

    if (timeoutMs <= 0) {
        lastError_ = performConnect();
        if (lastError_.isFailure() && nonBlocking_ && isConnectInProgressError(lastError_.getSystemCode())) {
            lastError_ = FASTNET_SUCCESS;
        }
        return lastError_;
    }

    const bool restoreBlocking = !nonBlocking_;
    if (restoreBlocking) {
        const Error nonBlockingResult = setNonBlocking(true);
        if (nonBlockingResult.isFailure()) {
            return nonBlockingResult;
        }
    }

    Error connectResult = performConnect();
    if (connectResult.isSuccess()) {
        if (restoreBlocking) {
            setNonBlocking(false);
        }
        lastError_ = FASTNET_SUCCESS;
        return lastError_;
    }

    const int connectError = connectResult.getSystemCode();
    if (!isConnectInProgressError(connectError)) {
        if (restoreBlocking) {
            setNonBlocking(false);
        }
        lastError_ = connectResult;
        return lastError_;
    }

    fd_set writeSet;
    fd_set errorSet;
    FD_ZERO(&writeSet);
    FD_ZERO(&errorSet);
    FD_SET(fd_, &writeSet);
    FD_SET(fd_, &errorSet);

    timeval timeout{};
    timeout.tv_sec = timeoutMs / 1000;
    timeout.tv_usec = (timeoutMs % 1000) * 1000;

    const int waitResult =
        ::select(static_cast<int>(fd_) + 1, nullptr, &writeSet, &errorSet, &timeout);
    if (waitResult <= 0) {
        if (restoreBlocking) {
            setNonBlocking(false);
        }
        lastError_ = waitResult == 0
                         ? Error(ErrorCode::TimeoutError, "Connection timed out")
                         : Error(ErrorCode::ConnectionError, "Failed while waiting for connection", getLastSocketError());
        return lastError_;
    }

    int socketError = 0;
    socket_len_t optionLength = static_cast<socket_len_t>(sizeof(socketError));
    if (::getsockopt(fd_,
                     SOL_SOCKET,
                     SO_ERROR,
                     reinterpret_cast<char*>(&socketError),
                     &optionLength) != 0) {
        if (restoreBlocking) {
            setNonBlocking(false);
        }
        lastError_ = Error(ErrorCode::ConnectionError, "Failed to inspect connection state", getLastSocketError());
        return lastError_;
    }

    if (restoreBlocking) {
        setNonBlocking(false);
    }

    if (socketError != 0) {
        lastError_ = Error(ErrorCode::ConnectionError, "Connection failed", socketError);
        return lastError_;
    }

    lastError_ = FASTNET_SUCCESS;
    return lastError_;
}

int SocketWrapper::send(const void* data, size_t len) {
    if (!isValid()) {
        lastError_ = FASTNET_ERROR(ErrorCode::InvalidArgument, "Invalid socket");
        return -1;
    }
    if (!isLengthRepresentable(len)) {
        lastError_ = FASTNET_ERROR(ErrorCode::InvalidArgument, "Send length exceeds platform limits");
        return -1;
    }
#ifdef _WIN32
    const int result = ::send(fd_, static_cast<const char*>(data), static_cast<int>(len), 0);
#else
    const int result = ::send(fd_, data, len, MSG_NOSIGNAL);
#endif
    lastError_ = result >= 0 ? FASTNET_SUCCESS : Error(ErrorCode::SocketError, "send failed", getLastSocketError());
    return result;
}

int SocketWrapper::recv(void* buffer, size_t len) {
    if (!isValid()) {
        lastError_ = FASTNET_ERROR(ErrorCode::InvalidArgument, "Invalid socket");
        return -1;
    }
    if (!isLengthRepresentable(len)) {
        lastError_ = FASTNET_ERROR(ErrorCode::InvalidArgument, "Receive length exceeds platform limits");
        return -1;
    }
#ifdef _WIN32
    const int result = ::recv(fd_, static_cast<char*>(buffer), static_cast<int>(len), 0);
#else
    const int result = ::recv(fd_, buffer, len, 0);
#endif
    lastError_ = result >= 0 ? FASTNET_SUCCESS : Error(ErrorCode::SocketError, "recv failed", getLastSocketError());
    return result;
}

int SocketWrapper::sendTo(const void* data, size_t len, const std::string& ip, uint16_t port) {
    return sendTo(data, len, Address(ip, port));
}

int SocketWrapper::sendTo(const void* data, size_t len, const Address& destination) {
    if (!isValid()) {
        lastError_ = FASTNET_ERROR(ErrorCode::InvalidArgument, "Invalid socket");
        return -1;
    }
    if (!isLengthRepresentable(len)) {
        lastError_ = FASTNET_ERROR(ErrorCode::InvalidArgument, "Send length exceeds platform limits");
        return -1;
    }
    if (type_ != SocketType::UDP) {
        lastError_ = FASTNET_ERROR(ErrorCode::InvalidArgument, "sendTo only applies to UDP sockets");
        return -1;
    }

    const auto resolvedAddresses = resolveAddresses(destination.host(), destination.port, SocketType::UDP, false, family_);
    if (!resolvedAddresses) {
        lastError_ = resolvedAddresses.error();
        return -1;
    }

    const ResolvedAddress* target = nullptr;
    for (const auto& candidate : resolvedAddresses.value()) {
        if (family_ == AF_UNSPEC || candidate.family == family_) {
            target = &candidate;
            break;
        }
    }

    if (target == nullptr) {
        lastError_ = Error(ErrorCode::ResolveError, "No resolved endpoint matched the socket family");
        return -1;
    }

#ifdef _WIN32
    const int result = ::sendto(fd_,
                                static_cast<const char*>(data),
                                static_cast<int>(len),
                                0,
                                reinterpret_cast<const sockaddr*>(&target->storage),
                                target->length);
#else
    const int result = ::sendto(fd_,
                                data,
                                len,
                                MSG_NOSIGNAL,
                                reinterpret_cast<const sockaddr*>(&target->storage),
                                target->length);
#endif
    lastError_ = result >= 0 ? FASTNET_SUCCESS : Error(ErrorCode::SocketError, "sendto failed", getLastSocketError());
    return result;
}

int SocketWrapper::recvFrom(void* buffer, size_t len, std::string& fromIp, uint16_t& fromPort) {
    if (!isValid()) {
        lastError_ = FASTNET_ERROR(ErrorCode::InvalidArgument, "Invalid socket");
        return -1;
    }
    if (!isLengthRepresentable(len)) {
        lastError_ = FASTNET_ERROR(ErrorCode::InvalidArgument, "Receive length exceeds platform limits");
        return -1;
    }

    sockaddr_storage fromAddress{};
    socket_len_t addressLength = static_cast<socket_len_t>(sizeof(fromAddress));

#ifdef _WIN32
    const int result = ::recvfrom(fd_,
                                  static_cast<char*>(buffer),
                                  static_cast<int>(len),
                                  0,
                                  reinterpret_cast<sockaddr*>(&fromAddress),
                                  &addressLength);
#else
    const int result = ::recvfrom(fd_,
                                  buffer,
                                  len,
                                  0,
                                  reinterpret_cast<sockaddr*>(&fromAddress),
                                  &addressLength);
#endif

    if (result >= 0) {
        const Error endpointError =
            endpointToText(reinterpret_cast<const sockaddr*>(&fromAddress), fromIp, fromPort);
        if (endpointError.isFailure()) {
            lastError_ = endpointError;
            return -1;
        }
        lastError_ = FASTNET_SUCCESS;
    } else {
        lastError_ = Error(ErrorCode::SocketError, "recvfrom failed", getLastSocketError());
    }
    return result;
}

void SocketWrapper::close() {
    if (fd_ != INVALID_SOCKET_FD) {
        ::closesocket(fd_);
        fd_ = INVALID_SOCKET_FD;
    }
    family_ = AF_UNSPEC;
    nonBlocking_ = false;
}

Error SocketWrapper::setNonBlocking(bool enable) {
    if (!isValid()) {
        lastError_ = FASTNET_ERROR(ErrorCode::InvalidArgument, "Invalid socket");
        return lastError_;
    }

#ifdef _WIN32
    u_long mode = enable ? 1 : 0;
    if (::ioctlsocket(fd_, FIONBIO, &mode) != 0) {
        lastError_ = Error(ErrorCode::SocketError, "Failed to set non-blocking mode", getLastSocketError());
        return lastError_;
    }
#else
    int flags = ::fcntl(fd_, F_GETFL, 0);
    if (flags == -1) {
        lastError_ = Error(ErrorCode::SocketError, "Failed to get socket flags", errno);
        return lastError_;
    }
    if (enable) {
        flags |= O_NONBLOCK;
    } else {
        flags &= ~O_NONBLOCK;
    }
    if (::fcntl(fd_, F_SETFL, flags) == -1) {
        lastError_ = Error(ErrorCode::SocketError, "Failed to set socket flags", errno);
        return lastError_;
    }
#endif

    nonBlocking_ = enable;
    lastError_ = FASTNET_SUCCESS;
    return lastError_;
}

Error SocketWrapper::setOption(const SocketOption& option) {
    if (!isValid()) {
        lastError_ = FASTNET_ERROR(ErrorCode::InvalidArgument, "Invalid socket");
        return lastError_;
    }

    if (!setReuseAddr(option.reuseAddr).isSuccess()) {
        return lastError_;
    }

#ifdef SO_REUSEPORT
    int reusePort = option.reusePort ? 1 : 0;
    if (::setsockopt(fd_,
                     SOL_SOCKET,
                     SO_REUSEPORT,
                     reinterpret_cast<const char*>(&reusePort),
                     static_cast<socket_len_t>(sizeof(reusePort))) != 0) {
        lastError_ = Error(ErrorCode::SocketError, "Failed to set SO_REUSEPORT", getLastSocketError());
        return lastError_;
    }
#endif

    if (type_ == SocketType::TCP) {
        if (!setKeepAlive(option.keepAlive).isSuccess()) {
            return lastError_;
        }
        if (!setNoDelay(option.noDelay).isSuccess()) {
            return lastError_;
        }
    }

    if (option.sendBufferSize > 0 && !setSendBufferSize(option.sendBufferSize).isSuccess()) {
        return lastError_;
    }
    if (option.recvBufferSize > 0 && !setRecvBufferSize(option.recvBufferSize).isSuccess()) {
        return lastError_;
    }
    if (option.sendTimeout > 0 && !setSendTimeout(option.sendTimeout).isSuccess()) {
        return lastError_;
    }
    if (option.recvTimeout > 0 && !setRecvTimeout(option.recvTimeout).isSuccess()) {
        return lastError_;
    }
    if (type_ == SocketType::UDP && !setBroadcast(option.broadcast).isSuccess()) {
        return lastError_;
    }

    lastError_ = FASTNET_SUCCESS;
    return lastError_;
}

Error SocketWrapper::setNoDelay(bool enable) {
    if (!isValid()) {
        lastError_ = FASTNET_ERROR(ErrorCode::InvalidArgument, "Invalid socket");
        return lastError_;
    }
    if (type_ != SocketType::TCP) {
        lastError_ = FASTNET_ERROR(ErrorCode::InvalidArgument, "TCP_NODELAY only applies to TCP sockets");
        return lastError_;
    }

    const int value = enable ? 1 : 0;
    if (::setsockopt(fd_,
                     IPPROTO_TCP,
                     TCP_NODELAY,
                     reinterpret_cast<const char*>(&value),
                     static_cast<socket_len_t>(sizeof(value))) != 0) {
        lastError_ = Error(ErrorCode::SocketError, "Failed to set TCP_NODELAY", getLastSocketError());
        return lastError_;
    }

    lastError_ = FASTNET_SUCCESS;
    return lastError_;
}

Error SocketWrapper::setReuseAddr(bool enable) {
    if (!isValid()) {
        lastError_ = FASTNET_ERROR(ErrorCode::InvalidArgument, "Invalid socket");
        return lastError_;
    }

    const int value = enable ? 1 : 0;
    if (::setsockopt(fd_,
                     SOL_SOCKET,
                     SO_REUSEADDR,
                     reinterpret_cast<const char*>(&value),
                     static_cast<socket_len_t>(sizeof(value))) != 0) {
        lastError_ = Error(ErrorCode::SocketError, "Failed to set SO_REUSEADDR", getLastSocketError());
        return lastError_;
    }

    lastError_ = FASTNET_SUCCESS;
    return lastError_;
}

Error SocketWrapper::setKeepAlive(bool enable) {
    if (!isValid()) {
        lastError_ = FASTNET_ERROR(ErrorCode::InvalidArgument, "Invalid socket");
        return lastError_;
    }

    const int value = enable ? 1 : 0;
    if (::setsockopt(fd_,
                     SOL_SOCKET,
                     SO_KEEPALIVE,
                     reinterpret_cast<const char*>(&value),
                     static_cast<socket_len_t>(sizeof(value))) != 0) {
        lastError_ = Error(ErrorCode::SocketError, "Failed to set SO_KEEPALIVE", getLastSocketError());
        return lastError_;
    }

    lastError_ = FASTNET_SUCCESS;
    return lastError_;
}

Error SocketWrapper::setSendTimeout(int timeoutMs) {
    if (!isValid()) {
        lastError_ = FASTNET_ERROR(ErrorCode::InvalidArgument, "Invalid socket");
        return lastError_;
    }

#ifdef _WIN32
    DWORD timeoutValue = static_cast<DWORD>((std::max)(timeoutMs, 0));
    if (::setsockopt(fd_,
                     SOL_SOCKET,
                     SO_SNDTIMEO,
                     reinterpret_cast<const char*>(&timeoutValue),
                     static_cast<socket_len_t>(sizeof(timeoutValue))) != 0) {
        lastError_ = Error(ErrorCode::SocketError, "Failed to set send timeout", getLastSocketError());
        return lastError_;
    }
#else
    timeval timeout{};
    timeout.tv_sec = (std::max)(timeoutMs, 0) / 1000;
    timeout.tv_usec = ((std::max)(timeoutMs, 0) % 1000) * 1000;
    if (::setsockopt(fd_,
                     SOL_SOCKET,
                     SO_SNDTIMEO,
                     reinterpret_cast<const char*>(&timeout),
                     static_cast<socket_len_t>(sizeof(timeout))) != 0) {
        lastError_ = Error(ErrorCode::SocketError, "Failed to set send timeout", errno);
        return lastError_;
    }
#endif

    lastError_ = FASTNET_SUCCESS;
    return lastError_;
}

Error SocketWrapper::setRecvTimeout(int timeoutMs) {
    if (!isValid()) {
        lastError_ = FASTNET_ERROR(ErrorCode::InvalidArgument, "Invalid socket");
        return lastError_;
    }

#ifdef _WIN32
    DWORD timeoutValue = static_cast<DWORD>((std::max)(timeoutMs, 0));
    if (::setsockopt(fd_,
                     SOL_SOCKET,
                     SO_RCVTIMEO,
                     reinterpret_cast<const char*>(&timeoutValue),
                     static_cast<socket_len_t>(sizeof(timeoutValue))) != 0) {
        lastError_ = Error(ErrorCode::SocketError, "Failed to set receive timeout", getLastSocketError());
        return lastError_;
    }
#else
    timeval timeout{};
    timeout.tv_sec  = (std::max)(timeoutMs, 0) / 1000;
    timeout.tv_usec = ((std::max)(timeoutMs, 0) % 1000) * 1000;
    if (::setsockopt(fd_,
                     SOL_SOCKET,
                     SO_RCVTIMEO,
                     reinterpret_cast<const char*>(&timeout),
                     static_cast<socket_len_t>(sizeof(timeout))) != 0) {
        lastError_ = Error(ErrorCode::SocketError, "Failed to set receive timeout", errno);
        return lastError_;
    }
#endif

    lastError_ = FASTNET_SUCCESS;
    return lastError_;
}

Error SocketWrapper::setSendBufferSize(int size) {
    if (!isValid()) {
        lastError_ = FASTNET_ERROR(ErrorCode::InvalidArgument, "Invalid socket");
        return lastError_;
    }

    if (::setsockopt(fd_,
                     SOL_SOCKET,
                     SO_SNDBUF,
                     reinterpret_cast<const char*>(&size),
                     static_cast<socket_len_t>(sizeof(size))) != 0) {
        lastError_ = Error(ErrorCode::SocketError, "Failed to set send buffer size", getLastSocketError());
        return lastError_;
    }

    lastError_ = FASTNET_SUCCESS;
    return lastError_;
}

Error SocketWrapper::setRecvBufferSize(int size) {
    if (!isValid()) {
        lastError_ = FASTNET_ERROR(ErrorCode::InvalidArgument, "Invalid socket");
        return lastError_;
    }

    if (::setsockopt(fd_,
                     SOL_SOCKET,
                     SO_RCVBUF,
                     reinterpret_cast<const char*>(&size),
                     static_cast<socket_len_t>(sizeof(size))) != 0) {
        lastError_ = Error(ErrorCode::SocketError, "Failed to set receive buffer size", getLastSocketError());
        return lastError_;
    }

    lastError_ = FASTNET_SUCCESS;
    return lastError_;
}

Error SocketWrapper::setBroadcast(bool enable) {
    if (!isValid()) {
        lastError_ = FASTNET_ERROR(ErrorCode::InvalidArgument, "Invalid socket");
        return lastError_;
    }
    if (type_ != SocketType::UDP) {
        lastError_ = FASTNET_ERROR(ErrorCode::InvalidArgument, "Broadcast only applies to UDP sockets");
        return lastError_;
    }

    const int value = enable ? 1 : 0;
    if (::setsockopt(fd_,
                     SOL_SOCKET,
                     SO_BROADCAST,
                     reinterpret_cast<const char*>(&value),
                     static_cast<socket_len_t>(sizeof(value))) != 0) {
        lastError_ = Error(ErrorCode::SocketError, "Failed to set broadcast", getLastSocketError());
        return lastError_;
    }

    lastError_ = FASTNET_SUCCESS;
    return lastError_;
}

void SocketWrapper::optimizeLoopbackFastPath() noexcept {
#ifdef _WIN32
#ifdef SIO_LOOPBACK_FAST_PATH
    if (!isValid() || type_ != SocketType::TCP) {
        return;
    }

    BOOL enabled = TRUE;
    DWORD bytesReturned = 0;
    if (::WSAIoctl(fd_,
                   SIO_LOOPBACK_FAST_PATH,
                   &enabled,
                   static_cast<DWORD>(sizeof(enabled)),
                   nullptr,
                   0,
                   &bytesReturned,
                   nullptr,
                   nullptr) == 0) {
        lastError_ = FASTNET_SUCCESS;
    }
#endif
#endif
}

Error SocketWrapper::shutdownWrite() {
    if (!isValid()) {
        lastError_ = FASTNET_ERROR(ErrorCode::InvalidArgument, "Invalid socket");
        return lastError_;
    }

#ifdef _WIN32
    if (::shutdown(fd_, SD_SEND) != 0) {
#else
    if (::shutdown(fd_, SHUT_WR) != 0) {
#endif
        lastError_ = Error(ErrorCode::SocketError, "Failed to shutdown write", getLastSocketError());
        return lastError_;
    }

    lastError_ = FASTNET_SUCCESS;
    return lastError_;
}

Error SocketWrapper::shutdownRead() {
    if (!isValid()) {
        lastError_ = FASTNET_ERROR(ErrorCode::InvalidArgument, "Invalid socket");
        return lastError_;
    }

#ifdef _WIN32
    if (::shutdown(fd_, SD_RECEIVE) != 0) {
#else
    if (::shutdown(fd_, SHUT_RD) != 0) {
#endif
        lastError_ = Error(ErrorCode::SocketError, "Failed to shutdown read", getLastSocketError());
        return lastError_;
    }

    lastError_ = FASTNET_SUCCESS;
    return lastError_;
}

Error SocketWrapper::shutdownBoth() {
    if (!isValid()) {
        lastError_ = FASTNET_ERROR(ErrorCode::InvalidArgument, "Invalid socket");
        return lastError_;
    }

#ifdef _WIN32
    if (::shutdown(fd_, SD_BOTH) != 0) {
#else
    if (::shutdown(fd_, SHUT_RDWR) != 0) {
#endif
        lastError_ = Error(ErrorCode::SocketError, "Failed to shutdown both", getLastSocketError());
        return lastError_;
    }

    lastError_ = FASTNET_SUCCESS;
    return lastError_;
}

Error SocketWrapper::getLocalAddress(std::string& ip, uint16_t& port) const {
    if (!isValid()) {
        return FASTNET_ERROR(ErrorCode::InvalidArgument, "Invalid socket");
    }

    sockaddr_storage address{};
    socket_len_t addressLength = static_cast<socket_len_t>(sizeof(address));
    if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&address), &addressLength) != 0) {
        return Error(ErrorCode::SocketError, "Failed to get local address", getLastSocketError());
    }
    return endpointToText(reinterpret_cast<const sockaddr*>(&address), ip, port);
}

Error SocketWrapper::getRemoteAddress(std::string& ip, uint16_t& port) const {
    if (!isValid()) {
        return FASTNET_ERROR(ErrorCode::InvalidArgument, "Invalid socket");
    }

    sockaddr_storage address{};
    socket_len_t addressLength = static_cast<socket_len_t>(sizeof(address));
    if (::getpeername(fd_, reinterpret_cast<sockaddr*>(&address), &addressLength) != 0) {
        return Error(ErrorCode::SocketError, "Failed to get remote address", getLastSocketError());
    }
    return endpointToText(reinterpret_cast<const sockaddr*>(&address), ip, port);
}

ssize_t SocketWrapper::sendFile(socket_t fileFd, off_t& offset, size_t count) {
    if (!isValid()) {
        return -1;
    }

#ifdef _WIN32
    if (count == 0) {
        LARGE_INTEGER fileSize{};
        if (!::GetFileSizeEx(reinterpret_cast<HANDLE>(fileFd), &fileSize)) {
            return -1;
        }
        const auto remaining = (std::max)(
            0LL,
            fileSize.QuadPart - static_cast<long long>(offset));
        count = static_cast<size_t>(remaining);
    }

    if (count > static_cast<size_t>((std::numeric_limits<DWORD>::max)())) {
        count = static_cast<size_t>((std::numeric_limits<DWORD>::max)());
    }
    if (count == 0) {
        return 0;
    }

    OVERLAPPED overlapped{};
    overlapped.Offset = static_cast<DWORD>(offset & 0xFFFFFFFF);
    overlapped.OffsetHigh = static_cast<DWORD>((static_cast<unsigned long long>(offset) >> 32) & 0xFFFFFFFF);

    if (!::TransmitFile(fd_,
                        reinterpret_cast<HANDLE>(fileFd),
                        static_cast<DWORD>(count),
                        0,
                        &overlapped,
                        nullptr,
                        TF_USE_KERNEL_APC)) {
        return -1;
    }

    offset += static_cast<off_t>(count);
    return static_cast<ssize_t>(count);
#else
    if (count == 0) {
        struct stat fileStat{};
        if (::fstat(fileFd, &fileStat) == -1) {
            return -1;
        }
        count = static_cast<size_t>(std::max<off_t>(0, fileStat.st_size - offset));
    }
    return ::sendfile(fd_, fileFd, &offset, count);
#endif
}

int SocketWrapper::getLastSocketError() {
    return SOCKET_ERROR_CODE;
}

std::string SocketWrapper::getErrorMessage(int errorCode) {
#ifdef _WIN32
    char* message = nullptr;
    ::FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                         FORMAT_MESSAGE_IGNORE_INSERTS,
                     nullptr,
                     static_cast<DWORD>(errorCode),
                     MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                     reinterpret_cast<LPSTR>(&message),
                     0,
                     nullptr);
    std::string result = message != nullptr ? message : "Unknown error";
    if (message != nullptr) {
        ::LocalFree(message);
    }
    return result;
#else
    return std::strerror(errorCode);
#endif
}

Error SocketWrapper::initializeSocketLibrary() {
#ifdef _WIN32
    if (g_wsaInitCount.fetch_add(1, std::memory_order_acq_rel) == 0) {
        WSADATA wsaData{};
        if (::WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            g_wsaInitCount.fetch_sub(1, std::memory_order_acq_rel);
            return FASTNET_SYSTEM_ERROR(ErrorCode::SocketError, "Failed to initialize Winsock");
        }
    }
#else
    std::signal(SIGPIPE, SIG_IGN);
#endif
    return FASTNET_SUCCESS;
}

void SocketWrapper::cleanupSocketLibrary() {
#ifdef _WIN32
    if (g_wsaInitCount.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        ::WSACleanup();
    }
#endif
}

int SocketWrapper::detectFamily() const noexcept {
    if (!isValid()) {
        return AF_UNSPEC;
    }

    sockaddr_storage address{};
    socket_len_t addressLength = static_cast<socket_len_t>(sizeof(address));
    if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&address), &addressLength) == 0) {
        return address.ss_family;
    }
    return AF_UNSPEC;
}

} // namespace FastNet
