/**
 * @file SocketWrapper.h
 * @brief FastNet native socket abstraction
 */
#pragma once

#include "Config.h"
#include "Error.h"

#include <cstddef>
#include <string>
#include <string_view>

#ifdef _WIN32
#ifndef FD_SETSIZE
#define FD_SETSIZE 4096
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <sys/types.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "mswsock.lib")
using socket_t = SOCKET;
using socket_len_t = int;
#define INVALID_SOCKET_FD INVALID_SOCKET
#define SOCKET_ERROR_CODE WSAGetLastError()
typedef __int64 ssize_t;
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
using socket_t = int;
using socket_len_t = socklen_t;
#define INVALID_SOCKET_FD (-1)
#define SOCKET_ERROR_CODE errno
#define closesocket close
#endif

namespace FastNet {

enum class SocketType {
    TCP,
    UDP
};

struct SocketOption {
    bool reuseAddr = false;
    bool reusePort = false;
    bool keepAlive = true;
    bool noDelay = true;
    int sendBufferSize = 0;
    int recvBufferSize = 0;
    int sendTimeout = 0;
    int recvTimeout = 0;
    bool broadcast = false;
};

class FASTNET_API SocketWrapper {
public:
    SocketWrapper() noexcept;
    explicit SocketWrapper(socket_t fd) noexcept;
    ~SocketWrapper();

    SocketWrapper(const SocketWrapper&) = delete;
    SocketWrapper& operator=(const SocketWrapper&) = delete;
    SocketWrapper(SocketWrapper&& other) noexcept;
    SocketWrapper& operator=(SocketWrapper&& other) noexcept;

    Error create(SocketType type);
    Error create(SocketType type, const Address& address, bool passive = false);
    Error create(SocketType type, std::string_view host, uint16_t port = 0, bool passive = false);

    Error bind(const std::string& ip, uint16_t port);
    Error bind(const Address& address);
    Error listen(int backlog = 128);
    Error accept(socket_t& clientFd, std::string& clientIp, uint16_t& clientPort);
    Error connect(const std::string& ip, uint16_t port, int timeoutMs = 0);
    Error connect(const Address& address, int timeoutMs = 0);

    int send(const void* data, size_t len);
    int recv(void* buffer, size_t len);
    int sendTo(const void* data, size_t len, const std::string& ip, uint16_t port);
    int sendTo(const void* data, size_t len, const Address& destination);
    int recvFrom(void* buffer, size_t len, std::string& fromIp, uint16_t& fromPort);

    void close();

    Error setNonBlocking(bool enable);
    Error setOption(const SocketOption& option);
    Error setNoDelay(bool enable);
    Error setReuseAddr(bool enable);
    Error setKeepAlive(bool enable);
    Error setSendTimeout(int timeoutMs);
    Error setRecvTimeout(int timeoutMs);
    Error setSendBufferSize(int size);
    Error setRecvBufferSize(int size);
    Error setBroadcast(bool enable);
    void optimizeLoopbackFastPath() noexcept;

    Error shutdownWrite();
    Error shutdownRead();
    Error shutdownBoth();

    Error getLocalAddress(std::string& ip, uint16_t& port) const;
    Error getRemoteAddress(std::string& ip, uint16_t& port) const;
    ssize_t sendFile(socket_t fileFd, off_t& offset, size_t count = 0);

    socket_t getFd() const noexcept { return fd_; }
    bool isValid() const noexcept { return fd_ != INVALID_SOCKET_FD; }
    SocketType getType() const noexcept { return type_; }
    int getFamily() const noexcept { return family_; }
    bool isNonBlocking() const noexcept { return nonBlocking_; }

    static int getLastSocketError();
    static std::string getErrorMessage(int errorCode);
    static Error initializeSocketLibrary();
    static void cleanupSocketLibrary();

    Error getLastError() const { return lastError_; }

private:
    Error createNativeSocket(int family);
    int detectFamily() const noexcept;

    socket_t fd_ = INVALID_SOCKET_FD;
    int family_ = AF_UNSPEC;
    SocketType type_ = SocketType::TCP;
    bool nonBlocking_ = false;
    Error lastError_;
};

} // namespace FastNet
