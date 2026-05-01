/**
 * @file HttpParser.h
 * @brief Lightweight HTTP message parsing and formatting helpers
 */
#pragma once

#include "Config.h"

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace FastNet {

struct FASTNET_API HttpRequestView {
    std::string_view method;
    std::string_view target;
    std::string_view uri;
    std::string_view queryString;
    std::string_view version;
    std::map<std::string_view, std::string_view> headers;
    std::string_view body;
    std::map<std::string_view, std::string_view> queryParams;

    void clear() noexcept;
    std::optional<std::string_view> getHeader(std::string_view name) const;
    size_t getContentLength() const;
    bool isKeepAlive() const;
};

struct FASTNET_API HttpResponseView {
    int statusCode = 200;
    std::string_view statusMessage = "OK";
    std::string_view version;
    std::map<std::string_view, std::string_view> headers;
    std::string_view body;

    void clear() noexcept;
    std::optional<std::string_view> getHeader(std::string_view name) const;
    size_t getContentLength() const;
    bool isKeepAlive() const;
};

class FASTNET_API HttpParser {
public:
    static bool parseRequestHead(std::string_view data, HttpRequestView& request);
    static bool parseRequest(std::string_view data, HttpRequestView& request);
    static bool parseResponseHead(std::string_view data, HttpResponseView& response);
    static bool parseResponse(std::string_view data, HttpResponseView& response);

    static void parseQueryString(std::string_view query,
                                 std::map<std::string_view, std::string_view>& params);

    static bool parseChunked(std::string_view data, std::string& body);
    static std::string buildChunked(std::string_view data);

    static std::string buildRequest(std::string_view method,
                                    std::string_view uri,
                                    const std::map<std::string, std::string>& headers,
                                    std::string_view body);

    static std::string buildResponse(int statusCode,
                                     std::string_view statusMessage,
                                     const std::map<std::string, std::string>& headers,
                                     std::string_view body);

    static std::string urlDecode(std::string_view encoded);
    static std::string urlEncode(std::string_view raw);

    static std::string_view trim(std::string_view str);
    static bool caseInsensitiveCompare(std::string_view a, std::string_view b);
};

} // namespace FastNet
