/**
 * @file Error.cpp
 * @brief FastNet error and exception model implementation
 */
#include "Error.h"

#include "Logger.h"

#include <cerrno>
#include <cstring>
#include <sstream>

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#endif

namespace FastNet {

namespace {

class FastNetErrorCategory final : public std::error_category {
public:
    const char* name() const noexcept override {
        return "fastnet";
    }

    std::string message(int condition) const override {
        return errorCodeToString(static_cast<ErrorCode>(condition));
    }
};

const FastNetErrorCategory kFastNetErrorCategory;

std::string trimSystemMessage(std::string message) {
    while (!message.empty() && (message.back() == '\r' || message.back() == '\n' || message.back() == ' ')) {
        message.pop_back();
    }
    return message;
}

} // namespace

Error::Error(ErrorCode code,
             std::string message,
             int systemCode,
             std::string fileName,
             int lineNumber,
             std::string functionName)
    : code_(code),
      message_(std::move(message)),
      systemCode_(systemCode),
      fileName_(std::move(fileName)),
      lineNumber_(lineNumber),
      functionName_(std::move(functionName)) {}

Error Error::success() {
    return Error();
}

Error Error::fromSystemError(ErrorCode code,
                             std::string message,
                             std::string fileName,
                             int lineNumber,
                             std::string functionName) {
    return Error(code,
                 std::move(message),
                 captureCurrentSystemCode(),
                 std::move(fileName),
                 lineNumber,
                 std::move(functionName));
}

ErrorCode Error::getCode() const noexcept {
    return code_;
}

const std::string& Error::getMessage() const noexcept {
    return message_;
}

int Error::getSystemCode() const noexcept {
    return systemCode_;
}

const std::string& Error::getFileName() const noexcept {
    return fileName_;
}

int Error::getLineNumber() const noexcept {
    return lineNumber_;
}

const std::string& Error::getFunctionName() const noexcept {
    return functionName_;
}

bool Error::isSuccess() const noexcept {
    return code_ == ErrorCode::Success;
}

bool Error::isFailure() const noexcept {
    return !isSuccess();
}

std::string Error::getSystemErrorMessage() const {
    if (systemCode_ == 0) {
        return {};
    }

#ifdef _WIN32
    char buffer[512] = {0};
    const DWORD flags = FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD size = FormatMessageA(flags,
                                      nullptr,
                                      static_cast<DWORD>(systemCode_),
                                      MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                      buffer,
                                      static_cast<DWORD>(sizeof(buffer)),
                                      nullptr);
    if (size == 0) {
        return {};
    }
    return trimSystemMessage(std::string(buffer, size));
#else
    return trimSystemMessage(std::strerror(systemCode_));
#endif
}

std::string Error::toString() const {
    std::ostringstream output;
    output << "Error [" << getErrorCodeName(code_) << "]";
    if (!message_.empty()) {
        output << ": " << message_;
    }

    if (systemCode_ != 0) {
        const std::string systemMessage = getSystemErrorMessage();
        if (!systemMessage.empty()) {
            output << " (system: " << systemMessage << " [" << systemCode_ << "])";
        } else {
            output << " (system code: " << systemCode_ << ")";
        }
    }

    if (!fileName_.empty() && lineNumber_ > 0) {
        output << " at " << fileName_ << ':' << lineNumber_;
        if (!functionName_.empty()) {
            output << " in " << functionName_;
        }
    }

    return output.str();
}

std::error_code Error::toStdErrorCode() const noexcept {
    return make_error_code(code_);
}

std::string Error::getErrorCodeName(ErrorCode code) {
    switch (code) {
        case ErrorCode::Success:
            return "Success";
        case ErrorCode::SocketError:
            return "SocketError";
        case ErrorCode::ConnectionError:
            return "ConnectionError";
        case ErrorCode::BindError:
            return "BindError";
        case ErrorCode::ListenError:
            return "ListenError";
        case ErrorCode::ResolveError:
            return "ResolveError";
        case ErrorCode::TimeoutError:
            return "TimeoutError";
        case ErrorCode::InvalidArgument:
            return "InvalidArgument";
        case ErrorCode::UnknownError:
            return "UnknownError";
        case ErrorCode::AlreadyRunning:
            return "AlreadyRunning";
        case ErrorCode::HttpProtocolError:
            return "HttpProtocolError";
        case ErrorCode::HttpResponseError:
            return "HttpResponseError";
        case ErrorCode::HttpRedirectError:
            return "HttpRedirectError";
        case ErrorCode::WebSocketHandshakeError:
            return "WebSocketHandshakeError";
        case ErrorCode::WebSocketProtocolError:
            return "WebSocketProtocolError";
        case ErrorCode::WebSocketFrameError:
            return "WebSocketFrameError";
        case ErrorCode::WebSocketPayloadTooLarge:
            return "WebSocketPayloadTooLarge";
        case ErrorCode::WebSocketCloseError:
            return "WebSocketCloseError";
        case ErrorCode::WebSocketPingPongTimeout:
            return "WebSocketPingPongTimeout";
        case ErrorCode::WebSocketConnectionError:
            return "WebSocketConnectionError";
        case ErrorCode::SSLError:
            return "SSLError";
        case ErrorCode::SSLHandshakeError:
            return "SSLHandshakeError";
        case ErrorCode::SSLCertificateError:
            return "SSLCertificateError";
        case ErrorCode::AuthenticationError:
            return "AuthenticationError";
        case ErrorCode::AuthorizationError:
            return "AuthorizationError";
        case ErrorCode::CompressionError:
            return "CompressionError";
        case ErrorCode::DecompressionError:
            return "DecompressionError";
        default:
            return "UnknownErrorCode";
    }
}

int Error::captureCurrentSystemCode() noexcept {
#ifdef _WIN32
    const int socketCode = ::WSAGetLastError();
    if (socketCode != 0) {
        return socketCode;
    }
    return static_cast<int>(::GetLastError());
#else
    return errno;
#endif
}

const std::error_category& fastnetErrorCategory() noexcept {
    return kFastNetErrorCategory;
}

std::error_code make_error_code(ErrorCode code) noexcept {
    return {static_cast<int>(code), fastnetErrorCategory()};
}

NetworkException::NetworkException(std::string message)
    : NetworkException(Error(ErrorCode::UnknownError, std::move(message))) {}

NetworkException::NetworkException(ErrorCode code, std::string message)
    : NetworkException(Error(code, std::move(message))) {}

NetworkException::NetworkException(Error error)
    : std::runtime_error(error.toString()),
      error_(std::move(error)) {}

NetworkException::~NetworkException() noexcept = default;

ErrorCode NetworkException::getCode() const noexcept {
    return error_.getCode();
}

const std::string& NetworkException::getMessage() const noexcept {
    return error_.getMessage();
}

const Error& NetworkException::getError() const noexcept {
    return error_;
}

SocketException::SocketException(std::string message)
    : NetworkException(Error(ErrorCode::SocketError, std::move(message))) {}

SocketException::SocketException(ErrorCode code, std::string message)
    : NetworkException(Error(code, std::move(message))) {}

SocketException::SocketException(Error error)
    : NetworkException(std::move(error)) {}

ConnectionException::ConnectionException(std::string message)
    : NetworkException(Error(ErrorCode::ConnectionError, std::move(message))) {}

ConnectionException::ConnectionException(ErrorCode code, std::string message)
    : NetworkException(Error(code, std::move(message))) {}

ConnectionException::ConnectionException(Error error)
    : NetworkException(std::move(error)) {}

ProtocolException::ProtocolException(std::string message)
    : NetworkException(Error(ErrorCode::HttpProtocolError, std::move(message))) {}

ProtocolException::ProtocolException(ErrorCode code, std::string message)
    : NetworkException(Error(code, std::move(message))) {}

ProtocolException::ProtocolException(Error error)
    : NetworkException(std::move(error)) {}

SSLException::SSLException(std::string message)
    : NetworkException(Error(ErrorCode::SSLError, std::move(message))) {}

SSLException::SSLException(ErrorCode code, std::string message)
    : NetworkException(Error(code, std::move(message))) {}

SSLException::SSLException(Error error)
    : NetworkException(std::move(error)) {}

TimeoutException::TimeoutException(std::string message)
    : NetworkException(Error(ErrorCode::TimeoutError, std::move(message))) {}

TimeoutException::TimeoutException(ErrorCode code, std::string message)
    : NetworkException(Error(code, std::move(message))) {}

TimeoutException::TimeoutException(Error error)
    : NetworkException(std::move(error)) {}

AuthenticationException::AuthenticationException(std::string message)
    : NetworkException(Error(ErrorCode::AuthenticationError, std::move(message))) {}

AuthenticationException::AuthenticationException(ErrorCode code, std::string message)
    : NetworkException(Error(code, std::move(message))) {}

AuthenticationException::AuthenticationException(Error error)
    : NetworkException(std::move(error)) {}

CompressionException::CompressionException(std::string message)
    : NetworkException(Error(ErrorCode::CompressionError, std::move(message))) {}

CompressionException::CompressionException(ErrorCode code, std::string message)
    : NetworkException(Error(code, std::move(message))) {}

CompressionException::CompressionException(Error error)
    : NetworkException(std::move(error)) {}

std::string errorCodeToString(ErrorCode code) {
    switch (code) {
        case ErrorCode::Success:
            return "Success";
        case ErrorCode::SocketError:
            return "Socket error";
        case ErrorCode::ConnectionError:
            return "Connection error";
        case ErrorCode::BindError:
            return "Bind error";
        case ErrorCode::ListenError:
            return "Listen error";
        case ErrorCode::ResolveError:
            return "Address resolution error";
        case ErrorCode::TimeoutError:
            return "Timeout error";
        case ErrorCode::InvalidArgument:
            return "Invalid argument";
        case ErrorCode::UnknownError:
            return "Unknown error";
        case ErrorCode::AlreadyRunning:
            return "Already running";
        case ErrorCode::HttpProtocolError:
            return "HTTP protocol error";
        case ErrorCode::HttpResponseError:
            return "HTTP response error";
        case ErrorCode::HttpRedirectError:
            return "HTTP redirect error";
        case ErrorCode::WebSocketHandshakeError:
            return "WebSocket handshake error";
        case ErrorCode::WebSocketProtocolError:
            return "WebSocket protocol error";
        case ErrorCode::WebSocketFrameError:
            return "WebSocket frame error";
        case ErrorCode::WebSocketPayloadTooLarge:
            return "WebSocket payload too large";
        case ErrorCode::WebSocketCloseError:
            return "WebSocket close error";
        case ErrorCode::WebSocketPingPongTimeout:
            return "WebSocket ping or pong timeout";
        case ErrorCode::WebSocketConnectionError:
            return "WebSocket connection error";
        case ErrorCode::SSLError:
            return "SSL error";
        case ErrorCode::SSLHandshakeError:
            return "SSL handshake error";
        case ErrorCode::SSLCertificateError:
            return "SSL certificate error";
        case ErrorCode::AuthenticationError:
            return "Authentication error";
        case ErrorCode::AuthorizationError:
            return "Authorization error";
        case ErrorCode::CompressionError:
            return "Compression error";
        case ErrorCode::DecompressionError:
            return "Decompression error";
        default:
            return "Unknown error code";
    }
}

ErrorCode systemErrorToNetworkError(int systemError) {
    switch (systemError) {
        case 0:
            return ErrorCode::Success;
#ifdef _WIN32
        case WSAETIMEDOUT:
        case ERROR_TIMEOUT:
            return ErrorCode::TimeoutError;
        case WSAECONNREFUSED:
        case WSAECONNRESET:
        case WSAENETRESET:
            return ErrorCode::ConnectionError;
        case WSAEADDRINUSE:
        case WSAEADDRNOTAVAIL:
            return ErrorCode::BindError;
        case WSAEHOSTUNREACH:
        case WSAENETUNREACH:
            return ErrorCode::ResolveError;
        case WSAEINVAL:
            return ErrorCode::InvalidArgument;
#else
        case ETIMEDOUT:
            return ErrorCode::TimeoutError;
        case ECONNREFUSED:
        case ECONNRESET:
        case ENOTCONN:
            return ErrorCode::ConnectionError;
        case EADDRINUSE:
        case EADDRNOTAVAIL:
            return ErrorCode::BindError;
        case ENETUNREACH:
        case EHOSTUNREACH:
            return ErrorCode::ResolveError;
        case EINVAL:
            return ErrorCode::InvalidArgument;
#endif
        default:
            return ErrorCode::UnknownError;
    }
}

ErrorCode boostErrorToNetworkError(const std::error_code& ec) {
    if (!ec) {
        return ErrorCode::Success;
    }
    if (ec.category() == fastnetErrorCategory()) {
        return static_cast<ErrorCode>(ec.value());
    }
    return systemErrorToNetworkError(ec.value());
}

[[noreturn]] void throwNetworkException(ErrorCode code, const std::string& message) {
    throw NetworkException(code, message);
}

[[noreturn]] void throwSocketException(ErrorCode code, const std::string& message) {
    throw SocketException(code, message);
}

[[noreturn]] void throwConnectionException(ErrorCode code, const std::string& message) {
    throw ConnectionException(code, message);
}

[[noreturn]] void throwProtocolException(ErrorCode code, const std::string& message) {
    throw ProtocolException(code, message);
}

[[noreturn]] void throwSSLException(ErrorCode code, const std::string& message) {
    throw SSLException(code, message);
}

[[noreturn]] void throwTimeoutException(ErrorCode code, const std::string& message) {
    throw TimeoutException(code, message);
}

[[noreturn]] void throwAuthenticationException(ErrorCode code, const std::string& message) {
    throw AuthenticationException(code, message);
}

[[noreturn]] void throwCompressionException(ErrorCode code, const std::string& message) {
    throw CompressionException(code, message);
}

ExceptionPolicy& ExceptionPolicy::getInstance() {
    static ExceptionPolicy instance;
    return instance;
}

void ExceptionPolicy::setStrategy(Strategy strategy) noexcept {
    strategy_.store(strategy, std::memory_order_release);
}

ExceptionPolicy::Strategy ExceptionPolicy::getStrategy() const noexcept {
    return strategy_.load(std::memory_order_acquire);
}

void ExceptionPolicy::enableExceptions() noexcept {
    setStrategy(Strategy::ThrowException);
}

void ExceptionPolicy::disableExceptions() noexcept {
    setStrategy(Strategy::ReturnErrorCode);
}

bool ExceptionPolicy::shouldThrow() const noexcept {
    return getStrategy() == Strategy::ThrowException;
}

bool ExceptionPolicy::shouldLog() const noexcept {
    return getStrategy() == Strategy::LogAndContinue;
}

void ExceptionPolicy::handle(const Error& error) const {
    if (error.isSuccess()) {
        return;
    }

    switch (getStrategy()) {
        case Strategy::ThrowException:
            throw NetworkException(error);
        case Strategy::LogAndContinue:
            if (AsyncLogger::getInstance().isRunning()) {
                AsyncLogger::getInstance().log(LogLevel::ERROR_LVL, nullptr, 0, nullptr, error.toString());
            } else {
                consoleLog(LogLevel::ERROR_LVL, error.toString());
            }
            return;
        case Strategy::ReturnErrorCode:
        default:
            return;
    }
}

void handleError(const Error& error) {
    if (error.isFailure()) {
        ExceptionPolicy::getInstance().handle(error);
    }
}

} // namespace FastNet
