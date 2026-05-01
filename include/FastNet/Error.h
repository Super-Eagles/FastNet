/**
 * @file Error.h
 * @brief FastNet error and exception model
 */
#pragma once

#include "Config.h"

#include <atomic>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>

namespace FastNet {

class FASTNET_API Error {
public:
    Error() = default;
    Error(ErrorCode code,
          std::string message,
          int systemCode = 0,
          std::string fileName = {},
          int lineNumber = 0,
          std::string functionName = {});

    static Error success();
    static Error fromSystemError(ErrorCode code,
                                 std::string message,
                                 std::string fileName = {},
                                 int lineNumber = 0,
                                 std::string functionName = {});

    ErrorCode getCode() const noexcept;
    const std::string& getMessage() const noexcept;
    int getSystemCode() const noexcept;
    const std::string& getFileName() const noexcept;
    int getLineNumber() const noexcept;
    const std::string& getFunctionName() const noexcept;

    bool isSuccess() const noexcept;
    bool isFailure() const noexcept;

    std::string getSystemErrorMessage() const;
    std::string toString() const;
    std::error_code toStdErrorCode() const noexcept;

    static std::string getErrorCodeName(ErrorCode code);

private:
    static int captureCurrentSystemCode() noexcept;

    ErrorCode code_ = ErrorCode::Success;
    std::string message_;
    int systemCode_ = 0;
    std::string fileName_;
    int lineNumber_ = 0;
    std::string functionName_;
};

FASTNET_API const std::error_category& fastnetErrorCategory() noexcept;
FASTNET_API std::error_code make_error_code(ErrorCode code) noexcept;

class FASTNET_API NetworkException : public std::runtime_error {
public:
    explicit NetworkException(std::string message);
    NetworkException(ErrorCode code, std::string message);
    explicit NetworkException(Error error);
    ~NetworkException() noexcept override;

    ErrorCode getCode() const noexcept;
    const std::string& getMessage() const noexcept;
    const Error& getError() const noexcept;

private:
    Error error_;
};

class FASTNET_API SocketException : public NetworkException {
public:
    explicit SocketException(std::string message);
    SocketException(ErrorCode code, std::string message);
    explicit SocketException(Error error);
};

class FASTNET_API ConnectionException : public NetworkException {
public:
    explicit ConnectionException(std::string message);
    ConnectionException(ErrorCode code, std::string message);
    explicit ConnectionException(Error error);
};

class FASTNET_API ProtocolException : public NetworkException {
public:
    explicit ProtocolException(std::string message);
    ProtocolException(ErrorCode code, std::string message);
    explicit ProtocolException(Error error);
};

class FASTNET_API SSLException : public NetworkException {
public:
    explicit SSLException(std::string message);
    SSLException(ErrorCode code, std::string message);
    explicit SSLException(Error error);
};

class FASTNET_API TimeoutException : public NetworkException {
public:
    explicit TimeoutException(std::string message);
    TimeoutException(ErrorCode code, std::string message);
    explicit TimeoutException(Error error);
};

class FASTNET_API AuthenticationException : public NetworkException {
public:
    explicit AuthenticationException(std::string message);
    AuthenticationException(ErrorCode code, std::string message);
    explicit AuthenticationException(Error error);
};

class FASTNET_API CompressionException : public NetworkException {
public:
    explicit CompressionException(std::string message);
    CompressionException(ErrorCode code, std::string message);
    explicit CompressionException(Error error);
};

FASTNET_API std::string errorCodeToString(ErrorCode code);
FASTNET_API ErrorCode systemErrorToNetworkError(int systemError);
FASTNET_API ErrorCode boostErrorToNetworkError(const std::error_code& ec);

[[noreturn]] FASTNET_API void throwNetworkException(ErrorCode code, const std::string& message);
[[noreturn]] FASTNET_API void throwSocketException(ErrorCode code, const std::string& message);
[[noreturn]] FASTNET_API void throwConnectionException(ErrorCode code, const std::string& message);
[[noreturn]] FASTNET_API void throwProtocolException(ErrorCode code, const std::string& message);
[[noreturn]] FASTNET_API void throwSSLException(ErrorCode code, const std::string& message);
[[noreturn]] FASTNET_API void throwTimeoutException(ErrorCode code, const std::string& message);
[[noreturn]] FASTNET_API void throwAuthenticationException(ErrorCode code, const std::string& message);
[[noreturn]] FASTNET_API void throwCompressionException(ErrorCode code, const std::string& message);

class FASTNET_API ExceptionPolicy {
public:
    enum class Strategy {
        ThrowException,
        ReturnErrorCode,
        LogAndContinue
    };

    static ExceptionPolicy& getInstance();

    void setStrategy(Strategy strategy) noexcept;
    Strategy getStrategy() const noexcept;

    void enableExceptions() noexcept;
    void disableExceptions() noexcept;
    bool shouldThrow() const noexcept;
    bool shouldLog() const noexcept;
    void handle(const Error& error) const;

private:
    ExceptionPolicy() = default;

    std::atomic<Strategy> strategy_{Strategy::ThrowException};
};

FASTNET_API void handleError(const Error& error);

template<typename T>
class Result {
public:
    static Result success(T value) {
        return Result(std::in_place_index<0>, std::move(value));
    }

    static Result error(Error error) {
        return Result(std::in_place_index<1>, std::move(error));
    }

    static Result error(ErrorCode code, std::string message = {}) {
        return error(Error(code, std::move(message)));
    }

    bool isSuccess() const noexcept {
        return std::holds_alternative<T>(storage_);
    }

    bool isError() const noexcept {
        return !isSuccess();
    }

    explicit operator bool() const noexcept {
        return isSuccess();
    }

    const T& value() const& {
        return std::get<T>(storage_);
    }

    T& value() & {
        return std::get<T>(storage_);
    }

    T&& value() && {
        return std::move(std::get<T>(storage_));
    }

    const Error& error() const& {
        return std::get<Error>(storage_);
    }

    ErrorCode errorCode() const noexcept {
        return isSuccess() ? ErrorCode::Success : std::get<Error>(storage_).getCode();
    }

    const std::string& errorMessage() const noexcept {
        static const std::string empty;
        return isSuccess() ? empty : std::get<Error>(storage_).getMessage();
    }

    const Error* errorIfAny() const noexcept {
        return isSuccess() ? nullptr : &std::get<Error>(storage_);
    }

    T valueOr(T defaultValue) const& {
        return isSuccess() ? value() : std::move(defaultValue);
    }

    T valueOr(T defaultValue) && {
        return isSuccess() ? std::move(value()) : std::move(defaultValue);
    }

private:
    template<size_t Index, typename... Args>
    explicit Result(std::in_place_index_t<Index>, Args&&... args)
        : storage_(std::in_place_index<Index>, std::forward<Args>(args)...) {}

    std::variant<T, Error> storage_;
};

template<>
class Result<void> {
public:
    static Result success() {
        return Result();
    }

    static Result error(Error error) {
        Result result;
        result.error_ = std::move(error);
        return result;
    }

    static Result error(ErrorCode code, std::string message = {}) {
        return error(Error(code, std::move(message)));
    }

    bool isSuccess() const noexcept {
        return !error_.has_value();
    }

    bool isError() const noexcept {
        return error_.has_value();
    }

    explicit operator bool() const noexcept {
        return isSuccess();
    }

    const Error& error() const {
        // Defensive: calling error() on a successful result is a logic bug.
        // In debug builds, this will assert. In release it returns a sentinel.
        if (!error_.has_value()) {
            static const Error kNoError;
            return kNoError;
        }
        return *error_;
    }

    ErrorCode errorCode() const noexcept {
        return error_ ? error_->getCode() : ErrorCode::Success;
    }

    const std::string& errorMessage() const noexcept {
        static const std::string empty;
        return error_ ? error_->getMessage() : empty;
    }

    const Error* errorIfAny() const noexcept {
        return error_ ? &*error_ : nullptr;
    }

private:
    Result() = default;

    std::optional<Error> error_;
};

} // namespace FastNet

namespace std {
template<>
struct is_error_code_enum<FastNet::ErrorCode> : true_type {};
} // namespace std

#define FASTNET_ERROR(code, message) \
    ::FastNet::Error((code), (message), 0, __FILE__, __LINE__, __FUNCTION__)
#define FASTNET_SYSTEM_ERROR(code, message) \
    ::FastNet::Error::fromSystemError((code), (message), __FILE__, __LINE__, __FUNCTION__)
#define FASTNET_SUCCESS \
    ::FastNet::Error()
