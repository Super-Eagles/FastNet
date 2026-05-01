#pragma once

#ifdef FASTNET_ENABLE_SSL

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>

namespace FastNetExamples {

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

inline TempTlsFiles makeTempTlsFiles(const std::string& prefix) {
    const auto baseDir = std::filesystem::current_path() / "tmp";
    std::error_code ec;
    std::filesystem::create_directories(baseDir, ec);

    const auto uniqueSuffix =
        std::to_string(static_cast<long long>(std::chrono::steady_clock::now().time_since_epoch().count()));
    TempTlsFiles files;
    files.directory = baseDir / (prefix + "-" + uniqueSuffix);
    std::filesystem::create_directories(files.directory, ec);
    files.certificate = files.directory / "server.crt";
    files.privateKey = files.directory / "server.key";
    return files;
}

inline bool writeSelfSignedCertificate(const TempTlsFiles& files, std::string& errorMessage) {
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

} // namespace FastNetExamples

#endif
