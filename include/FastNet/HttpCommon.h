/**
 * @file HttpCommon.h
 * @brief Shared HTTP types for FastNet
 */
#pragma once

#include "Config.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace FastNet {

using HttpHeaders = std::map<std::string, std::string>;

enum class HttpMethod {
    GET,
    POST,
    PUT,
    DELETE_METHOD,
    HEAD,
    OPTIONS_METHOD,
    PATCH
};

struct HttpResponse {
    int statusCode = 200;
    std::string statusMessage = "OK";
    HttpHeaders headers;
    std::string body;

    // File-backed responses allow large payloads to bypass full body buffering.
    bool hasFileBody = false;
    std::string filePath;
    uint64_t fileOffset = 0;
    uint64_t fileLength = 0;
};

struct HttpMultipartPart {
    std::string name;
    std::string fileName;
    std::string contentType;
    HttpHeaders headers;
    std::string body;
};

class HttpMultipartBuilder {
public:
    explicit HttpMultipartBuilder(std::string boundary = makeBoundary())
        : boundary_(std::move(boundary)) {}

    HttpMultipartBuilder& addField(std::string name, std::string value) {
        HttpMultipartPart part;
        part.name = std::move(name);
        part.body = std::move(value);
        parts_.push_back(std::move(part));
        return *this;
    }

    HttpMultipartBuilder& addFile(std::string name,
                                  std::string fileName,
                                  std::string body,
                                  std::string contentType = "application/octet-stream") {
        HttpMultipartPart part;
        part.name = std::move(name);
        part.fileName = std::move(fileName);
        part.contentType = std::move(contentType);
        part.body = std::move(body);
        parts_.push_back(std::move(part));
        return *this;
    }

    HttpMultipartBuilder& addPart(HttpMultipartPart part) {
        parts_.push_back(std::move(part));
        return *this;
    }

    const std::string& boundary() const noexcept {
        return boundary_;
    }

    std::string contentType() const {
        return "multipart/form-data; boundary=" + boundary_;
    }

    HttpHeaders headers() const {
        return {{"Content-Type", contentType()}};
    }

    std::string build() const {
        std::string body;
        for (const auto& part : parts_) {
            body += "--";
            body += boundary_;
            body += "\r\nContent-Disposition: form-data; name=\"";
            appendQuoted(body, part.name);
            body += "\"";
            if (!part.fileName.empty()) {
                body += "; filename=\"";
                appendQuoted(body, part.fileName);
                body += "\"";
            }
            body += "\r\n";
            if (!part.contentType.empty()) {
                body += "Content-Type: ";
                body += part.contentType;
                body += "\r\n";
            }
            for (const auto& [name, value] : part.headers) {
                body += name;
                body += ": ";
                body += value;
                body += "\r\n";
            }
            body += "\r\n";
            body += part.body;
            body += "\r\n";
        }
        body += "--";
        body += boundary_;
        body += "--\r\n";
        return body;
    }

private:
    static std::string makeBoundary() {
        static std::atomic<uint64_t> counter{0};
        const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        return "----FastNetBoundary" + std::to_string(static_cast<unsigned long long>(now)) +
               std::to_string(static_cast<unsigned long long>(counter.fetch_add(1, std::memory_order_relaxed)));
    }

    static void appendQuoted(std::string& output, std::string_view value) {
        for (char ch : value) {
            if (ch == '\\' || ch == '"') {
                output.push_back('\\');
            }
            output.push_back(ch);
        }
    }

    std::string boundary_;
    std::vector<HttpMultipartPart> parts_;
};

} // namespace FastNet
