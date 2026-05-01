#pragma once

#include "Config.h"
#include "Error.h"
#include "FastBuffer.h"

#include <memory>
#include <string>
#include <string_view>

#ifdef FASTNET_ENABLE_SSL
#include <openssl/err.h>
#include <openssl/x509_vfy.h>
#endif

namespace FastNet {

enum class WaitDirection {
    None,
    Read,
    Write
};

enum class TlsState {
    Disabled,
    Handshaking,
    Ready
};

enum class IoStatus {
    Ok,
    WouldBlock,
    Closed,
    Error
};

struct IoResult {
    IoStatus status = IoStatus::Ok;
    size_t bytes = 0;
    WaitDirection wait = WaitDirection::None;
    ErrorCode errorCode = ErrorCode::Success;
    std::string message;
};

struct QueuedWrite {
    Buffer bufferStorage;
    std::shared_ptr<const Buffer> bufferPayload;
    std::string stringStorage;
    std::shared_ptr<const std::string> stringPayload;

    size_t size() const noexcept {
        if (bufferPayload) {
            return bufferPayload->size();
        }
        if (!bufferStorage.empty()) {
            return bufferStorage.size();
        }
        if (stringPayload) {
            return stringPayload->size();
        }
        return stringStorage.size();
    }

    bool empty() const noexcept {
        return size() == 0;
    }

    const std::uint8_t* bytes() const noexcept {
        if (bufferPayload && !bufferPayload->empty()) {
            return bufferPayload->data();
        }
        if (!bufferStorage.empty()) {
            return bufferStorage.data();
        }
        if (stringPayload && !stringPayload->empty()) {
            return reinterpret_cast<const std::uint8_t*>(stringPayload->data());
        }
        if (!stringStorage.empty()) {
            return reinterpret_cast<const std::uint8_t*>(stringStorage.data());
        }
        return nullptr;
    }
};

inline bool isSocketWouldBlock(int error) {
#ifdef _WIN32
    return error == WSAEWOULDBLOCK || error == WSA_IO_PENDING;
#else
    return error == EAGAIN || error == EWOULDBLOCK || error == EINPROGRESS;
#endif
}

#ifdef FASTNET_ENABLE_SSL
inline std::string consumeOpenSslError() {
    std::string message;
    bool first = true;
    for (unsigned long error = ERR_get_error(); error != 0; error = ERR_get_error()) {
        char buffer[256] = {0};
        ERR_error_string_n(error, buffer, sizeof(buffer));
        if (!first) {
            message += "; ";
        }
        message += buffer;
        first = false;
    }
    return message;
}

inline std::string formatOpenSslFailure(const std::string& prefix) {
    const std::string details = consumeOpenSslError();
    return details.empty() ? prefix : prefix + ": " + details;
}

inline std::string formatVerifyFailure(long verifyResult) {
    const char* verifyMessage = X509_verify_cert_error_string(verifyResult);
    if (verifyMessage == nullptr || *verifyMessage == '\0') {
        return "TLS peer verification failed";
    }
    return std::string("TLS peer verification failed: ") + verifyMessage;
}
#endif

} // namespace FastNet
