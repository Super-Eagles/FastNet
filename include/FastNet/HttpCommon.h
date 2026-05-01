/**
 * @file HttpCommon.h
 * @brief Shared HTTP types for FastNet
 */
#pragma once

#include "Config.h"

#include <cstdint>
#include <map>
#include <string>

namespace FastNet {

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
    std::map<std::string, std::string> headers;
    std::string body;

    // File-backed responses allow large payloads to bypass full body buffering.
    bool hasFileBody = false;
    std::string filePath;
    uint64_t fileOffset = 0;
    uint64_t fileLength = 0;
};

} // namespace FastNet
