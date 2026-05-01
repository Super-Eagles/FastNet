/**
 * @file SSLContext.h
 * @brief FastNet TLS context wrapper
 */
#pragma once

#include "Config.h"
#include "Error.h"

#include <string>

#ifdef FASTNET_ENABLE_SSL
#include <openssl/err.h>
#include <openssl/ssl.h>
#endif

namespace FastNet {

class FASTNET_API SSLContext {
public:
    enum class Mode {
        Client,
        Server
    };

    SSLContext();
    ~SSLContext();

    SSLContext(const SSLContext&) = delete;
    SSLContext& operator=(const SSLContext&) = delete;
    SSLContext(SSLContext&&) = delete;
    SSLContext& operator=(SSLContext&&) = delete;

    bool initialize(const SSLConfig& sslConfig, Mode mode = Mode::Client);
    bool isInitialized() const;
    Mode getMode() const noexcept;
    const SSLConfig& getConfig() const noexcept;
    const Error& getLastError() const noexcept;
    std::string getLastErrorString() const;
    void cleanup();

#ifdef FASTNET_ENABLE_SSL
    SSL_CTX* getContext() const noexcept;
    SSL* createHandle() const;
#endif

private:
    void setLastError(Error error) const;
    void setLastError(ErrorCode code, const std::string& message) const;

#ifdef FASTNET_ENABLE_SSL
    static void ensureOpenSSLInitialized();
#endif

    bool initialized_ = false;
    Mode mode_ = Mode::Client;
    SSLConfig sslConfig_;
    mutable Error lastError_;

#ifdef FASTNET_ENABLE_SSL
    SSL_CTX* ctx_ = nullptr;
#endif
};

} // namespace FastNet
