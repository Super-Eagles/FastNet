/**
 * @file SSLContext.cpp
 * @brief FastNet TLS context wrapper implementation
 */
#include "SSLContext.h"

#include <sstream>
#include <mutex>
#include <utility>

#ifdef FASTNET_ENABLE_SSL
#include <openssl/x509_vfy.h>
#endif

namespace FastNet {

namespace {

#ifdef FASTNET_ENABLE_SSL
std::string captureOpenSslErrors() {
    std::ostringstream output;
    bool first = true;
    for (unsigned long errorCode = ERR_get_error(); errorCode != 0; errorCode = ERR_get_error()) {
        char buffer[256] = {0};
        ERR_error_string_n(errorCode, buffer, sizeof(buffer));
        if (!first) {
            output << " | ";
        }
        output << buffer;
        first = false;
    }
    return output.str();
}
#endif

} // namespace

SSLContext::SSLContext() = default;

SSLContext::~SSLContext() {
    cleanup();
}

bool SSLContext::initialize(const SSLConfig& sslConfig, Mode mode) {
    cleanup();
    sslConfig_ = sslConfig;
    mode_ = mode;
    lastError_ = Error::success();

    if (!sslConfig.enableSSL) {
        setLastError(ErrorCode::InvalidArgument, "SSL is disabled in SSLConfig");
        return false;
    }

#ifdef FASTNET_ENABLE_SSL
    ensureOpenSSLInitialized();

    const SSL_METHOD* method = (mode == Mode::Server) ? TLS_server_method() : TLS_client_method();
    ctx_ = SSL_CTX_new(method);
    if (ctx_ == nullptr) {
        setLastError(ErrorCode::SSLError, "Failed to create SSL_CTX");
        return false;
    }

    long options = SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 | SSL_OP_NO_COMPRESSION;
#ifdef SSL_OP_NO_RENEGOTIATION
    options |= SSL_OP_NO_RENEGOTIATION;
#endif
    SSL_CTX_set_options(ctx_, options);
    SSL_CTX_set_mode(ctx_, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);

    const int verifyMode = sslConfig.verifyPeer
                               ? ((mode == Mode::Server) ? (SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT)
                                                         : SSL_VERIFY_PEER)
                               : SSL_VERIFY_NONE;
    SSL_CTX_set_verify(ctx_, verifyMode, nullptr);

    if (!sslConfig.caFile.empty()) {
        if (SSL_CTX_load_verify_locations(ctx_, sslConfig.caFile.c_str(), nullptr) != 1) {
            setLastError(ErrorCode::SSLCertificateError, "Failed to load CA file");
            cleanup();
            return false;
        }
    } else if (sslConfig.verifyPeer) {
        if (SSL_CTX_set_default_verify_paths(ctx_) != 1) {
            setLastError(ErrorCode::SSLCertificateError, "Failed to load default CA paths");
            cleanup();
            return false;
        }
    }

    if (!sslConfig.certificateFile.empty()) {
        if (SSL_CTX_use_certificate_chain_file(ctx_, sslConfig.certificateFile.c_str()) != 1) {
            setLastError(ErrorCode::SSLCertificateError, "Failed to load certificate chain");
            cleanup();
            return false;
        }
    }

    if (!sslConfig.privateKeyFile.empty()) {
        if (SSL_CTX_use_PrivateKey_file(ctx_, sslConfig.privateKeyFile.c_str(), SSL_FILETYPE_PEM) != 1) {
            setLastError(ErrorCode::SSLCertificateError, "Failed to load private key");
            cleanup();
            return false;
        }
    }

    if (!sslConfig.certificateFile.empty() && !sslConfig.privateKeyFile.empty()) {
        if (SSL_CTX_check_private_key(ctx_) != 1) {
            setLastError(ErrorCode::SSLCertificateError, "Certificate and private key do not match");
            cleanup();
            return false;
        }
    }

    if (mode == Mode::Server) {
#ifdef SSL_OP_CIPHER_SERVER_PREFERENCE
        SSL_CTX_set_options(ctx_, SSL_OP_CIPHER_SERVER_PREFERENCE);
#endif
        SSL_CTX_set_session_cache_mode(ctx_, SSL_SESS_CACHE_SERVER);
    } else {
        SSL_CTX_set_session_cache_mode(ctx_, SSL_SESS_CACHE_CLIENT);
    }

    initialized_ = true;
    return true;
#else
    setLastError(ErrorCode::SSLError, "FastNet was built without FASTNET_ENABLE_SSL");
    return false;
#endif
}

bool SSLContext::isInitialized() const {
    return initialized_;
}

SSLContext::Mode SSLContext::getMode() const noexcept {
    return mode_;
}

const SSLConfig& SSLContext::getConfig() const noexcept {
    return sslConfig_;
}

const Error& SSLContext::getLastError() const noexcept {
    return lastError_;
}

std::string SSLContext::getLastErrorString() const {
    return lastError_.isFailure() ? lastError_.toString() : std::string();
}

void SSLContext::cleanup() {
#ifdef FASTNET_ENABLE_SSL
    if (ctx_ != nullptr) {
        SSL_CTX_free(ctx_);
        ctx_ = nullptr;
    }
#endif
    initialized_ = false;
}

#ifdef FASTNET_ENABLE_SSL
SSL_CTX* SSLContext::getContext() const noexcept {
    return ctx_;
}

SSL* SSLContext::createHandle() const {
    if (!initialized_ || ctx_ == nullptr) {
        setLastError(ErrorCode::InvalidArgument, "SSLContext is not initialized");
        return nullptr;
    }

    SSL* ssl = SSL_new(ctx_);
    if (ssl == nullptr) {
        setLastError(ErrorCode::SSLError, "Failed to allocate SSL handle");
        return nullptr;
    }

    if (mode_ == Mode::Server) {
        SSL_set_accept_state(ssl);
    } else {
        SSL_set_connect_state(ssl);
        if (sslConfig_.verifyPeer && !sslConfig_.hostnameVerification.empty()) {
#if OPENSSL_VERSION_NUMBER >= 0x10100000L
            if (Address::isValidIPv4(sslConfig_.hostnameVerification)) {
                X509_VERIFY_PARAM* verifyParam = SSL_get0_param(ssl);
                if (verifyParam == nullptr ||
                    X509_VERIFY_PARAM_set1_ip_asc(verifyParam, sslConfig_.hostnameVerification.c_str()) != 1) {
                    setLastError(ErrorCode::SSLCertificateError,
                                 "Failed to configure IP-based peer verification");
                    SSL_free(ssl);
                    return nullptr;
                }
            } else if (SSL_set1_host(ssl, sslConfig_.hostnameVerification.c_str()) != 1) {
                setLastError(ErrorCode::SSLCertificateError,
                             "Failed to configure hostname-based peer verification");
                SSL_free(ssl);
                return nullptr;
            }
#endif
        }
    }

    return ssl;
}
#endif

void SSLContext::setLastError(Error error) const {
    lastError_ = std::move(error);
}

void SSLContext::setLastError(ErrorCode code, const std::string& message) const {
    std::string fullMessage = message;
#ifdef FASTNET_ENABLE_SSL
    const std::string sslErrors = captureOpenSslErrors();
    if (!sslErrors.empty()) {
        fullMessage += ": " + sslErrors;
    }
#endif
    setLastError(Error(code, std::move(fullMessage)));
}

#ifdef FASTNET_ENABLE_SSL
void SSLContext::ensureOpenSSLInitialized() {
    static std::once_flag once;
    std::call_once(once, []() {
        OPENSSL_init_ssl(0, nullptr);
        OPENSSL_init_crypto(0, nullptr);
        SSL_load_error_strings();
        OpenSSL_add_all_algorithms();
    });
}
#endif

} // namespace FastNet
