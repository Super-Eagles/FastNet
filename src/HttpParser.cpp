/**
 * @file HttpParser.cpp
 * @brief Lightweight HTTP message parsing and formatting helpers
 */
#include "HttpParser.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <limits>

namespace FastNet {

namespace {

constexpr size_t kMaxHeaderSectionSize = 64 * 1024;
constexpr size_t kMaxRequestLineSize = 4096;
constexpr size_t kMaxStatusLineSize = 4096;
constexpr size_t kMaxHeaderLineSize = 8192;
constexpr size_t kMaxHeaderCount = 128;
constexpr size_t kChunkedEncodeBlockSize = 8192;

bool isTokenCharacter(char ch) {
    switch (ch) {
        case '!':
        case '#':
        case '$':
        case '%':
        case '&':
        case '\'':
        case '*':
        case '+':
        case '-':
        case '.':
        case '^':
        case '_':
        case '`':
        case '|':
        case '~':
            return true;
        default:
            return std::isalnum(static_cast<unsigned char>(ch)) != 0;
    }
}

bool isValidHeaderName(std::string_view headerName) {
    if (headerName.empty()) {
        return false;
    }
    return std::all_of(headerName.begin(), headerName.end(), isTokenCharacter);
}

bool isValidHttpVersion(std::string_view version) {
    if (version.size() != 8 || version.substr(0, 5) != "HTTP/") {
        return false;
    }
    return std::isdigit(static_cast<unsigned char>(version[5])) != 0 &&
           version[6] == '.' &&
           std::isdigit(static_cast<unsigned char>(version[7])) != 0;
}

std::optional<size_t> parseUnsignedDecimal(std::string_view value) {
    if (value.empty()) {
        return std::nullopt;
    }

    size_t result = 0;
    for (char ch : value) {
        if (std::isdigit(static_cast<unsigned char>(ch)) == 0) {
            return std::nullopt;
        }
        const size_t digit = static_cast<size_t>(ch - '0');
        if (result > ((std::numeric_limits<size_t>::max)() - digit) / 10) {
            return std::nullopt;
        }
        result = (result * 10) + digit;
    }

    return result;
}

std::optional<size_t> parseHexSize(std::string_view value) {
    value = HttpParser::trim(value);
    if (value.empty()) {
        return std::nullopt;
    }

    size_t result = 0;
    for (char ch : value) {
        unsigned int digit = 0;
        if (ch >= '0' && ch <= '9') {
            digit = static_cast<unsigned int>(ch - '0');
        } else if (ch >= 'A' && ch <= 'F') {
            digit = static_cast<unsigned int>(ch - 'A' + 10);
        } else if (ch >= 'a' && ch <= 'f') {
            digit = static_cast<unsigned int>(ch - 'a' + 10);
        } else {
            return std::nullopt;
        }

        if (result > (((std::numeric_limits<size_t>::max)() - digit) / 16)) {
            return std::nullopt;
        }
        result = (result * 16) + digit;
    }

    return result;
}

std::optional<size_t> contentLengthFromHeaders(const std::map<std::string_view, std::string_view>& headers) {
    for (const auto& [name, value] : headers) {
        if (HttpParser::caseInsensitiveCompare(name, "Content-Length")) {
            return parseUnsignedDecimal(HttpParser::trim(value));
        }
    }
    return std::nullopt;
}

bool containsToken(std::string_view headerValue, std::string_view token) {
    size_t offset = 0;
    while (offset <= headerValue.size()) {
        const size_t comma = headerValue.find(',', offset);
        const std::string_view current =
            comma == std::string_view::npos ? headerValue.substr(offset) : headerValue.substr(offset, comma - offset);
        if (HttpParser::caseInsensitiveCompare(HttpParser::trim(current), token)) {
            return true;
        }
        if (comma == std::string_view::npos) {
            break;
        }
        offset = comma + 1;
    }
    return false;
}

bool isChunkedEncoding(const std::map<std::string_view, std::string_view>& headers) {
    for (const auto& [name, value] : headers) {
        if (HttpParser::caseInsensitiveCompare(name, "Transfer-Encoding") && containsToken(value, "chunked")) {
            return true;
        }
    }
    return false;
}

bool assignMessageBody(std::string_view remaining,
                       const std::map<std::string_view, std::string_view>& headers,
                       std::string_view& body) {
    if (isChunkedEncoding(headers)) {
        body = remaining;
        return true;
    }

    const auto contentLength = contentLengthFromHeaders(headers);
    if (!contentLength.has_value()) {
        body = remaining;
        return true;
    }
    if (*contentLength > remaining.size()) {
        return false;
    }

    body = remaining.substr(0, *contentLength);
    return true;
}

bool parseHeaderLines(std::string_view headerLines,
                      std::map<std::string_view, std::string_view>& headers) {
    headers.clear();
    if (headerLines.empty()) {
        return true;
    }

    size_t offset = 0;
    size_t headerCount = 0;
    bool sawContentLength = false;
    bool sawTransferEncoding = false;
    while (offset < headerLines.size()) {
        const size_t lineEnd = headerLines.find("\r\n", offset);
        const std::string_view line =
            lineEnd == std::string_view::npos ? headerLines.substr(offset) : headerLines.substr(offset, lineEnd - offset);
        if (line.empty() || line.size() > kMaxHeaderLineSize || line.front() == ' ' || line.front() == '\t') {
            return false;
        }

        const size_t colon = line.find(':');
        if (colon == std::string_view::npos || colon == 0) {
            return false;
        }

        const std::string_view name = HttpParser::trim(line.substr(0, colon));
        const std::string_view value = HttpParser::trim(line.substr(colon + 1));
        if (!isValidHeaderName(name)) {
            return false;
        }

        if (HttpParser::caseInsensitiveCompare(name, "Content-Length")) {
            if (sawContentLength || !parseUnsignedDecimal(value).has_value()) {
                return false;
            }
            sawContentLength = true;
        } else if (HttpParser::caseInsensitiveCompare(name, "Transfer-Encoding")) {
            if (sawTransferEncoding) {
                return false;
            }
            sawTransferEncoding = true;
        }
        if (sawContentLength && sawTransferEncoding) {
            return false;
        }

        headers[name] = value;
        ++headerCount;
        if (headerCount > kMaxHeaderCount) {
            return false;
        }

        if (lineEnd == std::string_view::npos) {
            break;
        }
        offset = lineEnd + 2;
    }

    return true;
}

bool hasNoResponseBody(int statusCode) {
    return (statusCode >= 100 && statusCode < 200) || statusCode == 204 || statusCode == 304;
}

void appendHeaderLine(std::string& output, std::string_view name, std::string_view value) {
    output.append(name.data(), name.size());
    output.append(": ", 2);
    output.append(value.data(), value.size());
    output.append("\r\n", 2);
}

void appendUnsignedDecimal(std::string& output, size_t value) {
    char buffer[32];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    if (result.ec == std::errc()) {
        output.append(buffer, result.ptr);
    }
}

void appendSignedDecimal(std::string& output, int value) {
    char buffer[32];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    if (result.ec == std::errc()) {
        output.append(buffer, result.ptr);
    }
}

void appendUnsignedHex(std::string& output, size_t value) {
    char buffer[32];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value, 16);
    if (result.ec == std::errc()) {
        output.append(buffer, result.ptr);
    }
}

size_t decimalDigitCount(size_t value) {
    size_t digits = 1;
    while (value >= 10) {
        value /= 10;
        ++digits;
    }
    return digits;
}

size_t decimalDigitCount(int value) {
    size_t digits = value < 0 ? 2 : 1;
    int remaining = value < 0 ? -value : value;
    while (remaining >= 10) {
        remaining /= 10;
        ++digits;
    }
    return digits;
}

size_t hexDigitCount(size_t value) {
    size_t digits = 1;
    while (value >= 16) {
        value /= 16;
        ++digits;
    }
    return digits;
}

size_t estimateHeaderBytes(const std::map<std::string, std::string>& headers) {
    size_t total = 0;
    for (const auto& [name, value] : headers) {
        total += name.size() + 2 + value.size() + 2;
    }
    return total;
}

void appendHeaders(std::string& output,
                   const std::map<std::string, std::string>& headers,
                   std::string_view body) {
    bool hasContentLength = false;
    bool hasTransferEncoding = false;

    for (const auto& [name, value] : headers) {
        if (HttpParser::caseInsensitiveCompare(name, "Content-Length")) {
            hasContentLength = true;
        } else if (HttpParser::caseInsensitiveCompare(name, "Transfer-Encoding")) {
            hasTransferEncoding = true;
        }
        appendHeaderLine(output, name, value);
    }

    if (!hasTransferEncoding && !hasContentLength) {
        output.append("Content-Length: ", 16);
        appendUnsignedDecimal(output, body.size());
        output.append("\r\n", 2);
    }
}

int hexDigitValue(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    return -1;
}

} // namespace

void HttpRequestView::clear() noexcept {
    method = {};
    target = {};
    uri = {};
    queryString = {};
    version = {};
    headers.clear();
    body = {};
    queryParams.clear();
}

std::optional<std::string_view> HttpRequestView::getHeader(std::string_view name) const {
    for (const auto& [headerName, value] : headers) {
        if (HttpParser::caseInsensitiveCompare(headerName, name)) {
            return value;
        }
    }
    return std::nullopt;
}

size_t HttpRequestView::getContentLength() const {
    const auto length = contentLengthFromHeaders(headers);
    return length.value_or(0);
}

bool HttpRequestView::isKeepAlive() const {
    const auto connection = getHeader("Connection");
    if (HttpParser::caseInsensitiveCompare(version, "HTTP/1.0")) {
        return connection.has_value() && containsToken(*connection, "keep-alive");
    }
    if (connection.has_value() && containsToken(*connection, "close")) {
        return false;
    }
    return true;
}

void HttpResponseView::clear() noexcept {
    statusCode = 200;
    statusMessage = "OK";
    version = {};
    headers.clear();
    body = {};
}

std::optional<std::string_view> HttpResponseView::getHeader(std::string_view name) const {
    for (const auto& [headerName, value] : headers) {
        if (HttpParser::caseInsensitiveCompare(headerName, name)) {
            return value;
        }
    }
    return std::nullopt;
}

size_t HttpResponseView::getContentLength() const {
    const auto length = contentLengthFromHeaders(headers);
    return length.value_or(0);
}

bool HttpResponseView::isKeepAlive() const {
    const auto connection = getHeader("Connection");
    if (HttpParser::caseInsensitiveCompare(version, "HTTP/1.0")) {
        return connection.has_value() && containsToken(*connection, "keep-alive");
    }
    if (connection.has_value() && containsToken(*connection, "close")) {
        return false;
    }
    return true;
}

bool HttpParser::parseRequest(std::string_view data, HttpRequestView& request) {
    if (!parseRequestHead(data, request)) {
        return false;
    }

    const size_t headerEnd = data.find("\r\n\r\n");
    const std::string_view remaining = data.substr(headerEnd + 4);
    if (!assignMessageBody(remaining, request.headers, request.body)) {
        request.clear();
        return false;
    }

    return true;
}

bool HttpParser::parseRequestHead(std::string_view data, HttpRequestView& request) {
    request.clear();
    if (data.empty()) {
        return false;
    }

    const size_t headerEnd = data.find("\r\n\r\n");
    if (headerEnd == std::string_view::npos || headerEnd > kMaxHeaderSectionSize) {
        return false;
    }

    const size_t requestLineEnd = data.find("\r\n");
    if (requestLineEnd == std::string_view::npos || requestLineEnd > kMaxRequestLineSize) {
        return false;
    }

    const std::string_view requestLine = data.substr(0, requestLineEnd);
    const size_t firstSpace = requestLine.find(' ');
    if (firstSpace == std::string_view::npos || firstSpace == 0) {
        return false;
    }

    const size_t secondSpace = requestLine.find(' ', firstSpace + 1);
    if (secondSpace == std::string_view::npos || secondSpace == firstSpace + 1) {
        return false;
    }

    if (requestLine.find(' ', secondSpace + 1) != std::string_view::npos) {
        return false;
    }

    request.method = requestLine.substr(0, firstSpace);
    request.target = requestLine.substr(firstSpace + 1, secondSpace - firstSpace - 1);
    request.version = requestLine.substr(secondSpace + 1);

    if (!isValidHeaderName(request.method) || request.target.empty() || !isValidHttpVersion(request.version)) {
        request.clear();
        return false;
    }

    const size_t fragmentPos = request.target.find('#');
    const std::string_view targetWithoutFragment =
        fragmentPos == std::string_view::npos ? request.target : request.target.substr(0, fragmentPos);
    const size_t queryPos = targetWithoutFragment.find('?');
    if (queryPos == std::string_view::npos) {
        request.uri = targetWithoutFragment;
    } else {
        request.uri = targetWithoutFragment.substr(0, queryPos);
        request.queryString = targetWithoutFragment.substr(queryPos + 1);
        parseQueryString(request.queryString, request.queryParams);
    }

    const size_t headerLinesOffset = requestLineEnd + 2;
    const std::string_view headerLines = data.substr(headerLinesOffset, headerEnd - headerLinesOffset);
    if (!parseHeaderLines(headerLines, request.headers)) {
        request.clear();
        return false;
    }

    request.body = {};
    return true;
}

bool HttpParser::parseResponse(std::string_view data, HttpResponseView& response) {
    if (!parseResponseHead(data, response)) {
        return false;
    }

    const size_t headerEnd = data.find("\r\n\r\n");
    const std::string_view remaining = data.substr(headerEnd + 4);
    if (hasNoResponseBody(response.statusCode)) {
        response.body = {};
        return true;
    }
    if (!assignMessageBody(remaining, response.headers, response.body)) {
        response.clear();
        return false;
    }

    return true;
}

bool HttpParser::parseResponseHead(std::string_view data, HttpResponseView& response) {
    response.clear();
    if (data.empty()) {
        return false;
    }

    const size_t headerEnd = data.find("\r\n\r\n");
    if (headerEnd == std::string_view::npos || headerEnd > kMaxHeaderSectionSize) {
        return false;
    }

    const size_t statusLineEnd = data.find("\r\n");
    if (statusLineEnd == std::string_view::npos || statusLineEnd > kMaxStatusLineSize) {
        return false;
    }

    const std::string_view statusLine = data.substr(0, statusLineEnd);
    const size_t firstSpace = statusLine.find(' ');
    if (firstSpace == std::string_view::npos || firstSpace == 0) {
        return false;
    }

    const size_t secondSpace = statusLine.find(' ', firstSpace + 1);
    if (secondSpace == std::string_view::npos || secondSpace == firstSpace + 1) {
        return false;
    }

    response.version = statusLine.substr(0, firstSpace);
    const std::string_view codeText = statusLine.substr(firstSpace + 1, secondSpace - firstSpace - 1);
    response.statusMessage = trim(statusLine.substr(secondSpace + 1));

    if (!isValidHttpVersion(response.version) || codeText.size() != 3) {
        response.clear();
        return false;
    }

    int statusCode = 0;
    for (char ch : codeText) {
        if (std::isdigit(static_cast<unsigned char>(ch)) == 0) {
            response.clear();
            return false;
        }
        statusCode = (statusCode * 10) + (ch - '0');
    }
    response.statusCode = statusCode;

    const size_t headerLinesOffset = statusLineEnd + 2;
    const std::string_view headerLines = data.substr(headerLinesOffset, headerEnd - headerLinesOffset);
    if (!parseHeaderLines(headerLines, response.headers)) {
        response.clear();
        return false;
    }
    response.body = {};
    return true;
}

void HttpParser::parseQueryString(std::string_view query,
                                  std::map<std::string_view, std::string_view>& params) {
    params.clear();
    if (!query.empty() && query.front() == '?') {
        query.remove_prefix(1);
    }

    size_t offset = 0;
    while (offset <= query.size()) {
        const size_t ampersand = query.find('&', offset);
        const std::string_view pair =
            ampersand == std::string_view::npos ? query.substr(offset) : query.substr(offset, ampersand - offset);
        if (!pair.empty()) {
            const size_t equals = pair.find('=');
            if (equals == std::string_view::npos) {
                params[pair] = {};
            } else {
                params[pair.substr(0, equals)] = pair.substr(equals + 1);
            }
        }

        if (ampersand == std::string_view::npos) {
            break;
        }
        offset = ampersand + 1;
    }
}

bool HttpParser::parseChunked(std::string_view data, std::string& body) {
    body.clear();

    size_t offset = 0;
    while (offset < data.size()) {
        const size_t lineEnd = data.find("\r\n", offset);
        if (lineEnd == std::string_view::npos) {
            return false;
        }

        std::string_view sizeLine = trim(data.substr(offset, lineEnd - offset));
        const size_t extensionPos = sizeLine.find(';');
        if (extensionPos != std::string_view::npos) {
            sizeLine = trim(sizeLine.substr(0, extensionPos));
        }

        const auto chunkSize = parseHexSize(sizeLine);
        if (!chunkSize.has_value()) {
            return false;
        }

        offset = lineEnd + 2;
        if (*chunkSize == 0) {
            while (true) {
                const size_t trailerEnd = data.find("\r\n", offset);
                if (trailerEnd == std::string_view::npos) {
                    return false;
                }
                const std::string_view trailerLine = data.substr(offset, trailerEnd - offset);
                offset = trailerEnd + 2;
                if (trailerLine.empty()) {
                    return offset == data.size();
                }
                const size_t colon = trailerLine.find(':');
                if (colon == std::string_view::npos || !isValidHeaderName(trim(trailerLine.substr(0, colon)))) {
                    return false;
                }
            }
        }

        if (offset + *chunkSize + 2 > data.size()) {
            return false;
        }

        body.append(data.data() + offset, *chunkSize);
        offset += *chunkSize;
        if (data.substr(offset, 2) != "\r\n") {
            return false;
        }
        offset += 2;
    }

    return false;
}

std::string HttpParser::buildChunked(std::string_view data) {
    const size_t chunkCount = data.empty() ? 1 : ((data.size() + kChunkedEncodeBlockSize - 1) / kChunkedEncodeBlockSize);
    std::string encoded;
    encoded.reserve(data.size() + (chunkCount * (hexDigitCount(kChunkedEncodeBlockSize) + 4)) + 5);

    size_t offset = 0;
    while (offset < data.size()) {
        const size_t chunkSize = std::min(kChunkedEncodeBlockSize, data.size() - offset);
        appendUnsignedHex(encoded, chunkSize);
        encoded.append("\r\n", 2);
        if (chunkSize > 0) {
            encoded.append(data.data() + offset, chunkSize);
        }
        encoded.append("\r\n", 2);
        offset += chunkSize;
    }

    encoded.append("0\r\n\r\n", 5);
    return encoded;
}

std::string HttpParser::buildRequest(std::string_view method,
                                     std::string_view uri,
                                     const std::map<std::string, std::string>& headers,
                                     std::string_view body) {
    std::string request;
    request.reserve(method.size() + uri.size() + body.size() + estimateHeaderBytes(headers) +
                    decimalDigitCount(body.size()) + 32);

    request.append(method.data(), method.size());
    request.push_back(' ');
    request.append(uri.data(), uri.size());
    request.append(" HTTP/1.1\r\n", 11);
    appendHeaders(request, headers, body);
    request.append("\r\n", 2);
    if (!body.empty()) {
        request.append(body.data(), body.size());
    }
    return request;
}

std::string HttpParser::buildResponse(int statusCode,
                                      std::string_view statusMessage,
                                      const std::map<std::string, std::string>& headers,
                                      std::string_view body) {
    std::string response;
    response.reserve(statusMessage.size() + body.size() + estimateHeaderBytes(headers) +
                     decimalDigitCount(body.size()) + decimalDigitCount(statusCode) + 24);

    response.append("HTTP/1.1 ", 9);
    appendSignedDecimal(response, statusCode);
    response.push_back(' ');
    response.append(statusMessage.data(), statusMessage.size());
    response.append("\r\n", 2);
    appendHeaders(response, headers, body);
    response.append("\r\n", 2);
    if (!body.empty()) {
        response.append(body.data(), body.size());
    }
    return response;
}

std::string HttpParser::urlDecode(std::string_view encoded) {
    std::string decoded;
    decoded.reserve(encoded.size());

    for (size_t index = 0; index < encoded.size(); ++index) {
        if (encoded[index] == '%' && index + 2 < encoded.size()) {
            const int high = hexDigitValue(encoded[index + 1]);
            const int low = hexDigitValue(encoded[index + 2]);
            if (high >= 0 && low >= 0) {
                decoded.push_back(static_cast<char>((high << 4) | low));
                index += 2;
                continue;
            }
        }

        decoded.push_back(encoded[index] == '+' ? ' ' : encoded[index]);
    }

    return decoded;
}

std::string HttpParser::urlEncode(std::string_view raw) {
    constexpr char kHexDigitsUpper[] = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(raw.size() * 3);

    for (unsigned char ch : raw) {
        if (std::isalnum(ch) != 0 || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            encoded.push_back(static_cast<char>(ch));
            continue;
        }
        encoded.push_back('%');
        encoded.push_back(kHexDigitsUpper[(ch >> 4) & 0x0F]);
        encoded.push_back(kHexDigitsUpper[ch & 0x0F]);
    }

    return encoded;
}

std::string_view HttpParser::trim(std::string_view str) {
    size_t begin = 0;
    while (begin < str.size() && std::isspace(static_cast<unsigned char>(str[begin])) != 0) {
        ++begin;
    }

    size_t end = str.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(str[end - 1])) != 0) {
        --end;
    }

    return str.substr(begin, end - begin);
}

bool HttpParser::caseInsensitiveCompare(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }

    for (size_t index = 0; index < a.size(); ++index) {
        if (std::tolower(static_cast<unsigned char>(a[index])) !=
            std::tolower(static_cast<unsigned char>(b[index]))) {
            return false;
        }
    }

    return true;
}

} // namespace FastNet
