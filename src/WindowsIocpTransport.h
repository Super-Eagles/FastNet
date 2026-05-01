#pragma once

#ifdef _WIN32

#include "Error.h"
#include "SocketWrapper.h"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace FastNet {

class WindowsIocpSocketHandler {
public:
    virtual ~WindowsIocpSocketHandler() = default;

    virtual void handleWindowsIocpRead(const std::uint8_t* data, size_t size) = 0;
    virtual void handleWindowsIocpReadClosed() = 0;
    virtual void handleWindowsIocpReadError(int systemError) = 0;
    virtual void handleWindowsIocpWriteCompleted(size_t bytesTransferred) = 0;
    virtual void handleWindowsIocpWriteError(int systemError) = 0;
};

class WindowsIocpSocketTransport : public std::enable_shared_from_this<WindowsIocpSocketTransport> {
public:
    enum class OperationKind {
        Read,
        Write
    };

    struct Operation {
        OVERLAPPED overlapped{};
        WSABUF wsabuf{};
        OperationKind kind = OperationKind::Read;
        WindowsIocpSocketTransport* owner = nullptr;
    };

    static std::shared_ptr<WindowsIocpSocketTransport> create(socket_t socketFd,
                                                              const std::shared_ptr<WindowsIocpSocketHandler>& handler,
                                                              size_t workerHint,
                                                              size_t readBufferSize = 16 * 1024);

    ~WindowsIocpSocketTransport();

    Error start();
    Error submitWrite(const std::uint8_t* data, size_t size);
    void stop();
    void dispatchCompletion(Operation* operation, bool success, DWORD transferred, DWORD error);

private:
    WindowsIocpSocketTransport(socket_t socketFd,
                               std::weak_ptr<WindowsIocpSocketHandler> handler,
                               size_t workerHint,
                               size_t readBufferSize);

    Error postRead();

    socket_t socketFd_ = INVALID_SOCKET_FD;
    std::weak_ptr<WindowsIocpSocketHandler> handler_;
    size_t workerHint_ = 0;
    std::vector<std::uint8_t> readBuffer_;
    Operation readOperation_{};
    Operation writeOperation_{};

    std::mutex mutex_;
    std::condition_variable condition_;
    std::shared_ptr<WindowsIocpSocketTransport> keepAlive_;
    size_t pendingOperations_ = 0;
    const std::uint8_t* writeData_ = nullptr;
    size_t writeSize_ = 0;
    bool associated_ = false;
    bool stopped_ = false;
    bool readPending_ = false;
    bool writePending_ = false;
};

void shutdownWindowsIocpTransport();

} // namespace FastNet

#endif
