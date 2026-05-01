#include "FastNet/FastNet.h"
#include "TestSupport.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#ifdef FASTNET_ENABLE_SSL
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#endif

namespace {

#ifdef FASTNET_ENABLE_SSL

struct TempTlsFiles {
    std::filesystem::path directory;
    std::filesystem::path certificate;
    std::filesystem::path privateKey;

    TempTlsFiles() = default;
    TempTlsFiles(const TempTlsFiles&) = delete;
    TempTlsFiles& operator=(const TempTlsFiles&) = delete;
    TempTlsFiles(TempTlsFiles&& other) noexcept
        : directory(std::move(other.directory)),
          certificate(std::move(other.certificate)),
          privateKey(std::move(other.privateKey)) {
        other.directory.clear();
        other.certificate.clear();
        other.privateKey.clear();
    }
    TempTlsFiles& operator=(TempTlsFiles&& other) noexcept {
        if (this == &other) {
            return *this;
        }
        directory = std::move(other.directory);
        certificate = std::move(other.certificate);
        privateKey = std::move(other.privateKey);
        other.directory.clear();
        other.certificate.clear();
        other.privateKey.clear();
        return *this;
    }

    ~TempTlsFiles() {
        std::error_code ec;
        if (!directory.empty()) {
            std::filesystem::remove_all(directory, ec);
        }
    }
};

TempTlsFiles makeTempTlsFiles() {
    const auto baseDir = std::filesystem::current_path() / "tmp";
    std::error_code ec;
    std::filesystem::create_directories(baseDir, ec);

    const auto uniqueSuffix =
        std::to_string(static_cast<long long>(std::chrono::steady_clock::now().time_since_epoch().count()));
    TempTlsFiles files;
    files.directory = baseDir / ("tls-transport-" + uniqueSuffix);
    std::filesystem::create_directories(files.directory, ec);
    files.certificate = files.directory / "server.crt";
    files.privateKey = files.directory / "server.key";
    return files;
}

bool writeSelfSignedCertificate(const TempTlsFiles& files, std::string& errorMessage) {
    EVP_PKEY_CTX* keyContext = nullptr;
    EVP_PKEY* key = nullptr;
    X509* certificate = nullptr;
    BIO* certBio = nullptr;
    BIO* keyBio = nullptr;
    bool success = false;

    do {
        keyContext = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
        if (keyContext == nullptr) {
            errorMessage = "Failed to allocate EVP_PKEY_CTX";
            break;
        }
        if (EVP_PKEY_keygen_init(keyContext) <= 0) {
            errorMessage = "EVP_PKEY_keygen_init failed";
            break;
        }
        if (EVP_PKEY_CTX_set_rsa_keygen_bits(keyContext, 2048) <= 0) {
            errorMessage = "EVP_PKEY_CTX_set_rsa_keygen_bits failed";
            break;
        }
        if (EVP_PKEY_keygen(keyContext, &key) <= 0 || key == nullptr) {
            errorMessage = "EVP_PKEY_keygen failed";
            break;
        }

        certificate = X509_new();
        if (certificate == nullptr) {
            errorMessage = "Failed to allocate X509 certificate";
            break;
        }

        if (X509_set_version(certificate, 2) != 1) {
            errorMessage = "X509_set_version failed";
            break;
        }
        if (ASN1_INTEGER_set(X509_get_serialNumber(certificate), 1) != 1) {
            errorMessage = "Failed to set certificate serial number";
            break;
        }
        if (X509_gmtime_adj(X509_get_notBefore(certificate), 0) == nullptr ||
            X509_gmtime_adj(X509_get_notAfter(certificate), 24 * 60 * 60) == nullptr) {
            errorMessage = "Failed to set certificate validity window";
            break;
        }
        if (X509_set_pubkey(certificate, key) != 1) {
            errorMessage = "X509_set_pubkey failed";
            break;
        }

        X509_NAME* subject = X509_get_subject_name(certificate);
        if (subject == nullptr ||
            X509_NAME_add_entry_by_txt(
                subject, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>("localhost"), -1, -1, 0) != 1) {
            errorMessage = "Failed to set certificate subject";
            break;
        }
        if (X509_set_issuer_name(certificate, subject) != 1) {
            errorMessage = "Failed to set certificate issuer";
            break;
        }
        if (X509_sign(certificate, key, EVP_sha256()) <= 0) {
            errorMessage = "X509_sign failed";
            break;
        }

        certBio = BIO_new(BIO_s_mem());
        if (certBio == nullptr || PEM_write_bio_X509(certBio, certificate) != 1) {
            errorMessage = "Failed to write certificate PEM";
            break;
        }

        keyBio = BIO_new(BIO_s_mem());
        if (keyBio == nullptr || PEM_write_bio_PrivateKey(keyBio, key, nullptr, nullptr, 0, nullptr, nullptr) != 1) {
            errorMessage = "Failed to write private key PEM";
            break;
        }

        BUF_MEM* certBuffer = nullptr;
        BUF_MEM* keyBuffer = nullptr;
        BIO_get_mem_ptr(certBio, &certBuffer);
        BIO_get_mem_ptr(keyBio, &keyBuffer);
        if (certBuffer == nullptr || keyBuffer == nullptr) {
            errorMessage = "Failed to extract certificate PEM buffer";
            break;
        }

        std::ofstream certStream(files.certificate, std::ios::binary | std::ios::trunc);
        if (!certStream.write(certBuffer->data, static_cast<std::streamsize>(certBuffer->length))) {
            errorMessage = "Failed to persist certificate PEM";
            break;
        }

        std::ofstream keyStream(files.privateKey, std::ios::binary | std::ios::trunc);
        if (!keyStream.write(keyBuffer->data, static_cast<std::streamsize>(keyBuffer->length))) {
            errorMessage = "Failed to persist private key PEM";
            break;
        }

        success = true;
    } while (false);

    if (certBio != nullptr) {
        BIO_free(certBio);
    }
    if (keyBio != nullptr) {
        BIO_free(keyBio);
    }
    if (certificate != nullptr) {
        X509_free(certificate);
    }
    if (key != nullptr) {
        EVP_PKEY_free(key);
    }
    if (keyContext != nullptr) {
        EVP_PKEY_CTX_free(keyContext);
    }

    return success;
}

struct ExchangeState {
    std::mutex mutex;
    std::condition_variable condition;
    bool connectCompleted = false;
    bool connectSucceeded = false;
    bool failed = false;
    bool receivedAll = false;
    std::string failureMessage;
    FastNet::Buffer received;
};

void markFailure(ExchangeState& state, std::string message) {
    std::lock_guard<std::mutex> lock(state.mutex);
    if (!state.failed) {
        state.failed = true;
        state.failureMessage = std::move(message);
    }
    state.condition.notify_all();
}

template<typename Predicate>
bool waitForState(ExchangeState& state, std::chrono::milliseconds timeout, Predicate&& predicate) {
    std::unique_lock<std::mutex> lock(state.mutex);
    return state.condition.wait_for(lock, timeout, [&]() { return predicate(state); });
}

#endif

} // namespace

int main() {
#ifndef FASTNET_ENABLE_SSL
    std::cout << "tls transport tests skipped" << '\n';
    return 0;
#else
    using namespace std::chrono_literals;
    try {
        const TempTlsFiles tlsFiles = makeTempTlsFiles();
        std::string certificateError;
        FASTNET_TEST_ASSERT_MSG(writeSelfSignedCertificate(tlsFiles, certificateError), certificateError);

        const FastNet::ErrorCode initCode = FastNet::initialize();
        FASTNET_TEST_ASSERT_EQ(initCode, FastNet::ErrorCode::Success);

        struct CleanupGuard {
            ~CleanupGuard() {
                FastNet::cleanup();
            }
        } cleanupGuard;

        auto& ioService = FastNet::getGlobalIoService();
        FastNet::TcpServer server(ioService);

        ExchangeState state;
        const std::string payload(96 * 1024, 't');

        server.setServerErrorCallback([&state](const FastNet::Error& error) {
            markFailure(state, error.toString());
        });
        server.setOwnedDataReceivedCallback([&server, &state](FastNet::ConnectionId clientId, FastNet::Buffer&& data) {
            const FastNet::Error sendResult = server.sendToClient(clientId, std::move(data));
            if (sendResult.isFailure()) {
                markFailure(state, sendResult.toString());
            }
        });

        FastNet::SSLConfig serverSsl;
        serverSsl.enableSSL = true;
        serverSsl.verifyPeer = false;
        serverSsl.certificateFile = tlsFiles.certificate.string();
        serverSsl.privateKeyFile = tlsFiles.privateKey.string();

        const FastNet::Error startResult = server.start(0, "127.0.0.1", serverSsl);
        FASTNET_TEST_ASSERT_MSG(startResult.isSuccess(), startResult.toString());
        const FastNet::Address listenAddress = server.getListenAddress();
        FASTNET_TEST_ASSERT_MSG(listenAddress.port != 0, "server should bind an ephemeral loopback port");

        FastNet::TcpClient client(ioService);
        client.setConnectTimeout(5000);
        client.setReadTimeout(5000);
        client.setWriteTimeout(5000);

        client.setErrorCallback([&state](FastNet::ErrorCode, const std::string& message) {
            markFailure(state, message);
        });
        client.setDisconnectCallback([&state](const std::string& reason) {
            std::lock_guard<std::mutex> lock(state.mutex);
            if (!state.receivedAll && !state.failed) {
                state.failed = true;
                state.failureMessage = "Disconnected before full TLS echo: " + reason;
            }
            state.condition.notify_all();
        });
        client.setDataReceivedCallback([&state, expected = payload.size()](const FastNet::Buffer& data) {
            std::lock_guard<std::mutex> lock(state.mutex);
            state.received.insert(state.received.end(), data.begin(), data.end());
            if (state.received.size() >= expected) {
                state.receivedAll = true;
            }
            state.condition.notify_all();
        });

        FastNet::SSLConfig clientSsl;
        clientSsl.enableSSL = true;
        clientSsl.verifyPeer = false;

        const bool connectIssued = client.connect(
            listenAddress,
            [&state, &client, &payload](bool success, const std::string& message) {
                {
                    std::lock_guard<std::mutex> lock(state.mutex);
                    state.connectCompleted = true;
                    state.connectSucceeded = success;
                    if (!success) {
                        state.failed = true;
                        state.failureMessage = message;
                    }
                }
                state.condition.notify_all();

                if (success) {
                    const bool sent = client.send(std::string(payload));
                    if (!sent) {
                        markFailure(state, "Client failed to queue TLS payload");
                    }
                }
            },
            clientSsl);
        FASTNET_TEST_ASSERT(connectIssued);

        FASTNET_TEST_ASSERT_MSG(
            waitForState(state, 5s, [](const ExchangeState& current) { return current.connectCompleted || current.failed; }),
            "Timed out waiting for TLS client connect");

        {
            std::lock_guard<std::mutex> lock(state.mutex);
            FASTNET_TEST_ASSERT_MSG(!state.failed, state.failureMessage);
            FASTNET_TEST_ASSERT(state.connectSucceeded);
        }

        FASTNET_TEST_ASSERT_MSG(
            waitForState(state, 8s, [&](const ExchangeState& current) { return current.receivedAll || current.failed; }),
            "Timed out waiting for TLS echo payload");

        {
            std::lock_guard<std::mutex> lock(state.mutex);
            FASTNET_TEST_ASSERT_MSG(!state.failed, state.failureMessage);
            FASTNET_TEST_ASSERT_EQ(state.received.size(), payload.size());
            FASTNET_TEST_ASSERT_MSG(std::equal(state.received.begin(), state.received.end(), payload.begin()),
                                    "TLS echo payload should match original bytes");
        }

        client.disconnectAfterPendingWrites();
        server.stop();

        std::cout << "tls transport tests passed" << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "tls transport test failed: " << error.what() << std::endl;
        return 1;
    }
#endif
}
