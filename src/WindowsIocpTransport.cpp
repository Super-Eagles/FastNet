#include "WindowsIocpTransport.h"

#ifdef _WIN32

#include <algorithm>
#include <chrono>
#include <limits>
#include <thread>
#include <utility>
#include <vector>

namespace FastNet {

namespace {

constexpr ULONG_PTR kTransportShutdownCompletionKey = 0xF4574E54;
thread_local bool g_insideIocpWorker = false;

bool associateSocketWithCompletionPort(HANDLE completionPort, socket_t socketFd) {
    return ::CreateIoCompletionPort(reinterpret_cast<HANDLE>(socketFd), completionPort, 0, 0) != nullptr;
}

bool isExpectedTransportShutdownError(DWORD error) {
    return error == ERROR_OPERATION_ABORTED ||
           error == WSA_OPERATION_ABORTED ||
           error == ERROR_NETNAME_DELETED ||
           error == ERROR_CONNECTION_ABORTED;
}

size_t resolveWorkerCount(size_t requestedCount) {
    const size_t hardwareCount = (std::max)(
        static_cast<size_t>(1),
        static_cast<size_t>(std::thread::hardware_concurrency()));
    return requestedCount == 0 ? hardwareCount : (std::max)(static_cast<size_t>(1), requestedCount);
}

class WindowsIocpEngine {
public:
    static WindowsIocpEngine& instance() {
        static WindowsIocpEngine engine;
        return engine;
    }

    Error ensureStarted(size_t workerHint) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (completionPort_ != nullptr) {
            return FASTNET_SUCCESS;
        }

        completionPort_ = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
        if (completionPort_ == nullptr) {
            return Error(ErrorCode::SocketError,
                         "Failed to create Windows IOCP transport port",
                         static_cast<int>(::GetLastError()));
        }

        const size_t workerCount = resolveWorkerCount(workerHint);
        try {
            workers_.reserve(workerCount);
            for (size_t i = 0; i < workerCount; ++i) {
                workers_.emplace_back([this, completionPort = completionPort_]() {
                    workerLoop(completionPort);
                });
            }
        } catch (...) {
            const int systemError = static_cast<int>(::GetLastError());
            for (size_t i = 0; i < workers_.size(); ++i) {
                ::PostQueuedCompletionStatus(completionPort_, 0, kTransportShutdownCompletionKey, nullptr);
            }
            for (auto& worker : workers_) {
                if (worker.joinable()) {
                    worker.join();
                }
            }
            workers_.clear();
            ::CloseHandle(completionPort_);
            completionPort_ = nullptr;
            return Error(ErrorCode::SocketError,
                         "Failed to create Windows IOCP transport workers",
                         systemError);
        }

        return FASTNET_SUCCESS;
    }

    HANDLE port() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return completionPort_;
    }

    void shutdown() {
        std::vector<std::thread> workers;
        HANDLE completionPort = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (completionPort_ == nullptr) {
                return;
            }
            completionPort = completionPort_;
            workers.swap(workers_);
            completionPort_ = nullptr;
        }

        for (size_t i = 0; i < workers.size(); ++i) {
            ::PostQueuedCompletionStatus(completionPort, 0, kTransportShutdownCompletionKey, nullptr);
        }
        for (auto& worker : workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        ::CloseHandle(completionPort);
    }

private:
    void workerLoop(HANDLE completionPort) {
        g_insideIocpWorker = true;
        while (true) {
            DWORD transferred = 0;
            ULONG_PTR completionKey = 0;
            LPOVERLAPPED overlapped = nullptr;
            const BOOL completed =
                ::GetQueuedCompletionStatus(completionPort, &transferred, &completionKey, &overlapped, INFINITE);
            const DWORD error = completed ? ERROR_SUCCESS : static_cast<DWORD>(::GetLastError());

            if (completionKey == kTransportShutdownCompletionKey && overlapped == nullptr) {
                break;
            }
            if (overlapped == nullptr) {
                continue;
            }

            auto* operation = reinterpret_cast<WindowsIocpSocketTransport::Operation*>(overlapped);
            if (operation->owner != nullptr) {
                operation->owner->dispatchCompletion(operation, completed == TRUE, transferred, error);
            }
        }
        g_insideIocpWorker = false;
    }

    mutable std::mutex mutex_;
    HANDLE completionPort_ = nullptr;
    std::vector<std::thread> workers_;
};

} // namespace

std::shared_ptr<WindowsIocpSocketTransport> WindowsIocpSocketTransport::create(
    socket_t socketFd,
    const std::shared_ptr<WindowsIocpSocketHandler>& handler,
    size_t workerHint,
    size_t readBufferSize) {
    return std::shared_ptr<WindowsIocpSocketTransport>(
        new WindowsIocpSocketTransport(socketFd, handler, workerHint, readBufferSize));
}

WindowsIocpSocketTransport::WindowsIocpSocketTransport(socket_t socketFd,
                                                       std::weak_ptr<WindowsIocpSocketHandler> handler,
                                                       size_t workerHint,
                                                       size_t readBufferSize)
    : socketFd_(socketFd),
      handler_(std::move(handler)),
      workerHint_(workerHint),
      readBuffer_((std::max)(static_cast<size_t>(1), readBufferSize)) {
    readOperation_.kind = OperationKind::Read;
    readOperation_.owner = this;
    writeOperation_.kind = OperationKind::Write;
    writeOperation_.owner = this;
}

WindowsIocpSocketTransport::~WindowsIocpSocketTransport() {
    stop();
}

Error WindowsIocpSocketTransport::start() {
    Error engineError = WindowsIocpEngine::instance().ensureStarted(workerHint_);
    if (engineError.isFailure()) {
        return engineError;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_) {
            return FASTNET_ERROR(ErrorCode::ConnectionError, "Windows IOCP transport is stopped");
        }
        if (!associated_) {
            HANDLE completionPort = WindowsIocpEngine::instance().port();
            if (completionPort == nullptr || !associateSocketWithCompletionPort(completionPort, socketFd_)) {
                return Error(ErrorCode::SocketError,
                             "Failed to associate socket with Windows IOCP transport",
                             static_cast<int>(::GetLastError()));
            }
            associated_ = true;
        }
    }

    return postRead();
}

Error WindowsIocpSocketTransport::submitWrite(const std::uint8_t* data, size_t size) {
    if (data == nullptr || size == 0) {
        return FASTNET_SUCCESS;
    }

    const size_t writeSize = (std::min)(size, static_cast<size_t>((std::numeric_limits<DWORD>::max)()));

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_) {
            return FASTNET_ERROR(ErrorCode::ConnectionError, "Windows IOCP transport is stopped");
        }
        if (writePending_) {
            return FASTNET_ERROR(ErrorCode::AlreadyRunning, "An overlapped write is already pending");
        }

        writePending_ = true;
        writeData_ = data;
        writeSize_ = writeSize;
        if (!keepAlive_) {
            keepAlive_ = shared_from_this();
        }
        ++pendingOperations_;

        writeOperation_.overlapped = OVERLAPPED{};
        writeOperation_.wsabuf.buf = reinterpret_cast<char*>(const_cast<std::uint8_t*>(writeData_));
        writeOperation_.wsabuf.len = static_cast<ULONG>(writeSize_);
    }

    DWORD bytesSent = 0;
    const int sendResult =
        ::WSASend(socketFd_, &writeOperation_.wsabuf, 1, &bytesSent, 0, &writeOperation_.overlapped, nullptr);
    if (sendResult == 0) {
        return FASTNET_SUCCESS;
    }

    const int systemError = ::WSAGetLastError();
    if (systemError == WSA_IO_PENDING) {
        return FASTNET_SUCCESS;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    writePending_ = false;
    writeData_ = nullptr;
    writeSize_ = 0;
    if (pendingOperations_ > 0) {
        --pendingOperations_;
    }
    if (stopped_ && pendingOperations_ == 0) {
        keepAlive_.reset();
    }
    condition_.notify_all();
    return Error(ErrorCode::SocketError, "WSASend failed", systemError);
}

void WindowsIocpSocketTransport::stop() {
    std::shared_ptr<WindowsIocpSocketTransport> keepAliveRelease;
    bool shouldWait = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_) {
            shouldWait = pendingOperations_ != 0 && !g_insideIocpWorker;
        } else {
            stopped_ = true;
            if (pendingOperations_ == 0) {
                keepAliveRelease = std::move(keepAlive_);
            } else {
                shouldWait = !g_insideIocpWorker;
            }
        }
    }

    if (socketFd_ != INVALID_SOCKET_FD) {
        ::CancelIoEx(reinterpret_cast<HANDLE>(socketFd_), nullptr);
    }

    if (shouldWait) {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait_for(lock, std::chrono::seconds(5), [this]() {
            return pendingOperations_ == 0;
        });
        if (pendingOperations_ == 0) {
            keepAliveRelease = std::move(keepAlive_);
        }
    }
}

Error WindowsIocpSocketTransport::postRead() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_ || readPending_) {
            return FASTNET_SUCCESS;
        }

        readPending_ = true;
        if (!keepAlive_) {
            keepAlive_ = shared_from_this();
        }
        ++pendingOperations_;

        readOperation_.overlapped = OVERLAPPED{};
        readOperation_.wsabuf.buf = reinterpret_cast<char*>(readBuffer_.data());
        readOperation_.wsabuf.len =
            static_cast<ULONG>((std::min)(readBuffer_.size(), static_cast<size_t>((std::numeric_limits<DWORD>::max)())));
    }

    DWORD flags = 0;
    DWORD bytesReceived = 0;
    const int recvResult = ::WSARecv(
        socketFd_, &readOperation_.wsabuf, 1, &bytesReceived, &flags, &readOperation_.overlapped, nullptr);
    if (recvResult == 0) {
        return FASTNET_SUCCESS;
    }

    const int systemError = ::WSAGetLastError();
    if (systemError == WSA_IO_PENDING) {
        return FASTNET_SUCCESS;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    readPending_ = false;
    if (pendingOperations_ > 0) {
        --pendingOperations_;
    }
    if (stopped_ && pendingOperations_ == 0) {
        keepAlive_.reset();
    }
    condition_.notify_all();
    return Error(ErrorCode::SocketError, "WSARecv failed", systemError);
}

void WindowsIocpSocketTransport::dispatchCompletion(Operation* operation,
                                                    bool success,
                                                    DWORD transferred,
                                                    DWORD error) {
    std::shared_ptr<WindowsIocpSocketTransport> lifetime;
    std::shared_ptr<WindowsIocpSocketHandler> handler;
    bool stopped = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        lifetime = keepAlive_;
        if (operation->kind == OperationKind::Read) {
            readPending_ = false;
        } else {
            writePending_ = false;
            writeData_ = nullptr;
            writeSize_ = 0;
        }
        stopped = stopped_;
        if (!stopped) {
            handler = handler_.lock();
        }
    }

    const bool suppressError = stopped && isExpectedTransportShutdownError(error);
    const bool shouldPostRead =
        !suppressError && handler && operation->kind == OperationKind::Read && success && transferred > 0;

    if (!suppressError && handler) {
        try {
            if (success) {
                if (operation->kind == OperationKind::Read) {
                    if (transferred == 0) {
                        handler->handleWindowsIocpReadClosed();
                    } else {
                        handler->handleWindowsIocpRead(readBuffer_.data(), static_cast<size_t>(transferred));
                    }
                } else {
                    handler->handleWindowsIocpWriteCompleted(static_cast<size_t>(transferred));
                }
            } else {
                if (operation->kind == OperationKind::Read) {
                    handler->handleWindowsIocpReadError(static_cast<int>(error));
                } else {
                    handler->handleWindowsIocpWriteError(static_cast<int>(error));
                }
            }
        } catch (...) {
            // Completion threads must remain recoverable.
        }
    }

    if (shouldPostRead) {
        const Error readError = postRead();
        if (readError.isFailure()) {
            auto readHandler = handler_.lock();
            if (readHandler) {
                try {
                    readHandler->handleWindowsIocpReadError(readError.getSystemCode());
                } catch (...) {
                    // Completion threads must remain recoverable.
                }
            }
        }
    }

    std::shared_ptr<WindowsIocpSocketTransport> keepAliveRelease;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pendingOperations_ > 0) {
            --pendingOperations_;
        }
        if (stopped_ && pendingOperations_ == 0) {
            keepAliveRelease = std::move(keepAlive_);
        }
    }
    condition_.notify_all();
}

void shutdownWindowsIocpTransport() {
    WindowsIocpEngine::instance().shutdown();
}

} // namespace FastNet

#endif
