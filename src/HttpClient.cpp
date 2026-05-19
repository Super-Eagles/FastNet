/**
 * @file HttpClient.cpp
 * @brief FastNet HTTP 客户端实现
 */
#include "HttpClient.h"

#include "Error.h"
#include "HttpParser.h"
#include "TcpClient.h"
#include "Timer.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstddef>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <utility>
#include <vector>

namespace FastNet {

namespace {

bool containsHeaderCaseInsensitive(const RequestHeaders& headers, std::string_view name) {
    return std::any_of(headers.begin(), headers.end(), [name](const auto& entry) {
        return HttpParser::caseInsensitiveCompare(entry.first, name);
    });
}

bool containsHeaderTokenCaseInsensitive(std::string_view headerValue, std::string_view token) {
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

bool supportsCompressedResponseDecoding() noexcept {
    return false;
}

bool isInterimResponseStatus(int statusCode) noexcept {
    return statusCode >= 100 && statusCode < 200 && statusCode != 101;
}

bool isValidHttpMethodName(std::string_view method) {
    if (method.empty()) {
        return false;
    }
    return std::all_of(method.begin(), method.end(), [](unsigned char ch) {
        return std::isalnum(ch) != 0 || ch == '-' || ch == '_';
    });
}

const std::string* findHeaderCaseInsensitive(const RequestHeaders& headers, std::string_view name) {
    for (const auto& entry : headers) {
        if (HttpParser::caseInsensitiveCompare(entry.first, name)) {
            return &entry.second;
        }
    }
    return nullptr;
}

void eraseHeaderCaseInsensitive(RequestHeaders& headers, std::string_view name) {
    for (auto it = headers.begin(); it != headers.end();) {
        if (HttpParser::caseInsensitiveCompare(it->first, name)) {
            it = headers.erase(it);
        } else {
            ++it;
        }
    }
}

std::string toLowerCopy(std::string_view value) {
    std::string lowered(value.begin(), value.end());
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return lowered;
}

void appendHeaderLine(std::string& request, std::string_view name, std::string_view value) {
    request.append(name.data(), name.size());
    request.append(": ", 2);
    request.append(value.data(), value.size());
    request.append("\r\n", 2);
}

void appendUnsignedDecimal(std::string& output, size_t value) {
    char buffer[32];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
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

struct RequestHeaderState {
    bool hasHost = false;
    bool hasUserAgent = false;
    bool hasConnection = false;
    bool hasAcceptEncoding = false;
    bool hasContentLength = false;
    bool hasTransferEncoding = false;
    size_t serializedBytes = 0;
};

RequestHeaderState scanRequestHeaders(const RequestHeaders& headers) {
    RequestHeaderState state;
    for (const auto& [name, value] : headers) {
        state.serializedBytes += name.size() + 2 + value.size() + 2;
        if (HttpParser::caseInsensitiveCompare(name, "Host")) {
            state.hasHost = true;
        } else if (HttpParser::caseInsensitiveCompare(name, "User-Agent")) {
            state.hasUserAgent = true;
        } else if (HttpParser::caseInsensitiveCompare(name, "Connection")) {
            state.hasConnection = true;
        } else if (HttpParser::caseInsensitiveCompare(name, "Accept-Encoding")) {
            state.hasAcceptEncoding = true;
        } else if (HttpParser::caseInsensitiveCompare(name, "Content-Length")) {
            state.hasContentLength = true;
        } else if (HttpParser::caseInsensitiveCompare(name, "Transfer-Encoding")) {
            state.hasTransferEncoding = true;
        }
    }
    return state;
}

std::string buildClientRequest(std::string_view method,
                               std::string_view path,
                               const RequestHeaders& headers,
                               std::string_view body,
                               std::string_view hostHeader,
                               bool useCompression) {
    const RequestHeaderState state = scanRequestHeaders(headers);
    const bool addContentLength = !state.hasContentLength && !state.hasTransferEncoding;
    const bool advertiseCompression = useCompression && supportsCompressedResponseDecoding();

    size_t reserveBytes = method.size() + path.size() + body.size() + state.serializedBytes + 16;
    if (!state.hasHost) {
        reserveBytes += sizeof("Host: \r\n") - 1 + hostHeader.size();
    }
    if (!state.hasUserAgent) {
        reserveBytes += sizeof("User-Agent: FastNet-HttpClient/2.0\r\n") - 1;
    }
    if (!state.hasConnection) {
        reserveBytes += sizeof("Connection: keep-alive\r\n") - 1;
    }
    if (advertiseCompression && !state.hasAcceptEncoding) {
        reserveBytes += sizeof("Accept-Encoding: gzip, deflate\r\n") - 1;
    }
    if (addContentLength) {
        reserveBytes += 18 + decimalDigitCount(body.size());
    }

    std::string request;
    request.reserve(reserveBytes);
    request.append(method.data(), method.size());
    request.push_back(' ');
    request.append(path.data(), path.size());
    request.append(" HTTP/1.1\r\n", 11);
    for (const auto& [name, value] : headers) {
        appendHeaderLine(request, name, value);
    }
    if (!state.hasHost) {
        appendHeaderLine(request, "Host", hostHeader);
    }
    if (!state.hasUserAgent) {
        appendHeaderLine(request, "User-Agent", "FastNet-HttpClient/2.0");
    }
    if (!state.hasConnection) {
        appendHeaderLine(request, "Connection", "keep-alive");
    }
    if (advertiseCompression && !state.hasAcceptEncoding) {
        appendHeaderLine(request, "Accept-Encoding", "gzip, deflate");
    }
    if (addContentLength) {
        request.append("Content-Length: ", 16);
        appendUnsignedDecimal(request, body.size());
        request.append("\r\n", 2);
    }
    request.append("\r\n", 2);
    if (!body.empty()) {
        request.append(body.data(), body.size());
    }
    return request;
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

std::optional<size_t> getChunkedMessageSize(std::string_view data) {
    size_t offset = 0;
    while (true) {
        const size_t lineEnd = data.find("\r\n", offset);
        if (lineEnd == std::string_view::npos) {
            return std::nullopt;
        }

        std::string_view sizeLine = HttpParser::trim(data.substr(offset, lineEnd - offset));
        const size_t extensionPos = sizeLine.find(';');
        if (extensionPos != std::string_view::npos) {
            sizeLine = HttpParser::trim(sizeLine.substr(0, extensionPos));
        }

        const auto chunkSize = parseHexSize(sizeLine);
        if (!chunkSize.has_value()) {
            return 0;
        }

        offset = lineEnd + 2;
        if (*chunkSize == 0) {
            while (true) {
                const size_t trailerEnd = data.find("\r\n", offset);
                if (trailerEnd == std::string_view::npos) {
                    return std::nullopt;
                }
                const std::string_view trailerLine = data.substr(offset, trailerEnd - offset);
                offset = trailerEnd + 2;
                if (trailerLine.empty()) {
                    return offset;
                }
                const size_t colon = trailerLine.find(':');
                if (colon == std::string_view::npos || colon == 0) {
                    return 0;
                }
            }
        }

        if (data.size() < offset + *chunkSize + 2) {
            return std::nullopt;
        }
        if (data.substr(offset + *chunkSize, 2) != "\r\n") {
            return 0;
        }
        offset += *chunkSize + 2;
    }
}

std::string normalizeHttpPath(std::string_view value) {
    std::string_view queryPart;
    const size_t queryPos = value.find('?');
    if (queryPos != std::string_view::npos) {
        queryPart = value.substr(queryPos);
        value = value.substr(0, queryPos);
    }

    const bool absolute = value.empty() || value.front() == '/';
    std::vector<std::string> segments;
    size_t offset = 0;
    while (offset <= value.size()) {
        const size_t slash = value.find('/', offset);
        const std::string_view segment =
            slash == std::string_view::npos ? value.substr(offset) : value.substr(offset, slash - offset);
        if (!segment.empty() && segment != ".") {
            if (segment == "..") {
                if (!segments.empty()) {
                    segments.pop_back();
                }
            } else {
                segments.emplace_back(segment);
            }
        }
        if (slash == std::string_view::npos) {
            break;
        }
        offset = slash + 1;
    }

    std::string normalized = absolute ? "/" : std::string();
    for (size_t index = 0; index < segments.size(); ++index) {
        if (index > 0) {
            normalized.push_back('/');
        }
        normalized += segments[index];
    }
    if (normalized.empty()) {
        normalized = "/";
    }
    normalized.append(queryPart.begin(), queryPart.end());
    return normalized;
}

std::string joinRelativePath(std::string_view basePath, std::string_view relativePath) {
    const size_t queryPos = basePath.find('?');
    if (queryPos != std::string_view::npos) {
        basePath = basePath.substr(0, queryPos);
    }

    if (basePath.empty() || basePath.front() != '/') {
        basePath = "/";
    }

    const size_t slashPos = basePath.rfind('/');
    const std::string_view baseDirectory =
        slashPos == std::string_view::npos ? std::string_view("/") : basePath.substr(0, slashPos + 1);
    return normalizeHttpPath(std::string(baseDirectory) + std::string(relativePath));
}

} // namespace

HttpClientRequest::HttpClientRequest(std::string path)
    : path_(path.empty() ? "/" : std::move(path)) {}

HttpClientRequest& HttpClientRequest::setMethod(std::string method) {
    method_ = std::move(method);
    return *this;
}

HttpClientRequest& HttpClientRequest::setPath(std::string path) {
    path_ = path.empty() ? "/" : std::move(path);
    return *this;
}

HttpClientRequest& HttpClientRequest::addHeader(std::string name, std::string value) {
    headers_[std::move(name)] = std::move(value);
    return *this;
}

HttpClientRequest& HttpClientRequest::addQuery(std::string name, std::string value) {
    const char separator = path_.find('?') == std::string::npos ? '?' : '&';
    path_.push_back(separator);
    path_ += HttpParser::urlEncode(name);
    path_.push_back('=');
    path_ += HttpParser::urlEncode(value);
    return *this;
}

HttpClientRequest& HttpClientRequest::setBody(std::string body) {
    body_ = std::move(body);
    return *this;
}

HttpClientRequest& HttpClientRequest::setJson(std::string json) {
    eraseHeaderCaseInsensitive(headers_, "Content-Type");
    headers_["Content-Type"] = "application/json";
    body_ = std::move(json);
    return *this;
}

HttpClientRequest& HttpClientRequest::setForm(const std::map<std::string, std::string>& values) {
    eraseHeaderCaseInsensitive(headers_, "Content-Type");
    headers_["Content-Type"] = "application/x-www-form-urlencoded";
    body_.clear();
    for (const auto& [name, value] : values) {
        if (!body_.empty()) {
            body_.push_back('&');
        }
        body_ += HttpParser::urlEncode(name);
        body_.push_back('=');
        body_ += HttpParser::urlEncode(value);
    }
    return *this;
}

HttpClientRequest& HttpClientRequest::setBearerToken(std::string token) {
    eraseHeaderCaseInsensitive(headers_, "Authorization");
    headers_["Authorization"] = "Bearer " + token;
    return *this;
}

HttpClientRequest& HttpClientRequest::setBasicAuth(std::string username, std::string password) {
    eraseHeaderCaseInsensitive(headers_, "Authorization");
    headers_["Authorization"] = "Basic " + base64Encode(username + ":" + password);
    return *this;
}

HttpClientRequest& HttpClientRequest::setUserAgent(std::string userAgent) {
    eraseHeaderCaseInsensitive(headers_, "User-Agent");
    headers_["User-Agent"] = std::move(userAgent);
    return *this;
}

HttpClientRequest& HttpClientRequest::setAccept(std::string accept) {
    eraseHeaderCaseInsensitive(headers_, "Accept");
    headers_["Accept"] = std::move(accept);
    return *this;
}

HttpClientRequest& HttpClientRequest::setTimeout(std::chrono::milliseconds timeout) {
    const auto count = timeout.count();
    timeoutMs_ = count <= 0 ? 0 : static_cast<uint32_t>(std::min<int64_t>(count, (std::numeric_limits<uint32_t>::max)()));
    return *this;
}

const std::string& HttpClientRequest::method() const noexcept {
    return method_;
}

const std::string& HttpClientRequest::path() const noexcept {
    return path_;
}

const RequestHeaders& HttpClientRequest::headers() const noexcept {
    return headers_;
}

const std::string& HttpClientRequest::body() const noexcept {
    return body_;
}

uint32_t HttpClientRequest::timeoutMs() const noexcept {
    return timeoutMs_;
}

class HttpClient::Impl : public std::enable_shared_from_this<HttpClient::Impl> {
public:
    explicit Impl(IoService& ioService)
        : ioService_(ioService),
          tcpClient_(ioService),
          requestTimer_(std::make_unique<Timer>(ioService)) {}

    ~Impl() {
        disconnect();
    }

    bool connect(std::string_view url, const HttpClientConnectCallback& callback) {
        clearLastError();
        ParsedUrl parsed;
        if (!parseUrl(url, parsed)) {
            setLastError(ErrorCode::InvalidArgument, "Invalid URL");
            if (callback) {
                callback(false, "Invalid URL");
            }
            return false;
        }

        connectCallback_ = callback;
        parsedUrl_ = parsed;
        redirectCount_ = 0;
        return connectToParsedUrl(false);
    }

    bool get(const std::string& path, const RequestHeaders& headers, const HttpClientResponseCallback& callback) {
        return sendRequest("GET", path, headers, "", callback);
    }

    bool head(const std::string& path, const RequestHeaders& headers, const HttpClientResponseCallback& callback) {
        return sendRequest("HEAD", path, headers, "", callback);
    }

    bool post(const std::string& path,
              const RequestHeaders& headers,
              const std::string& body,
              const HttpClientResponseCallback& callback) {
        return sendRequest("POST", path, headers, body, callback);
    }

    bool put(const std::string& path,
             const RequestHeaders& headers,
             const std::string& body,
             const HttpClientResponseCallback& callback) {
        return sendRequest("PUT", path, headers, body, callback);
    }

    bool patch(const std::string& path,
               const RequestHeaders& headers,
               const std::string& body,
               const HttpClientResponseCallback& callback) {
        return sendRequest("PATCH", path, headers, body, callback);
    }

    bool del(const std::string& path, const RequestHeaders& headers, const HttpClientResponseCallback& callback) {
        return sendRequest("DELETE", path, headers, "", callback);
    }

    bool request(std::string_view method,
                 std::string_view path,
                 const RequestHeaders& headers,
                 std::string_view body,
                 const HttpClientResponseCallback& callback) {
        return sendRequest(method, path, headers, body, callback);
    }

    bool request(const HttpClientRequest& request, const HttpClientResponseCallback& callback) {
        return sendRequestObject(request, callback);
    }

    bool streamRequest(std::string_view method,
                       std::string_view path,
                       const RequestHeaders& headers,
                       std::string_view body,
                       const HttpClientStreamHeadersCallback& headersCallback,
                       const HttpClientStreamDataCallback& dataCallback,
                       const HttpClientStreamCompleteCallback& completeCallback) {
        return sendStreamingRequest(method, path, headers, body, headersCallback, dataCallback, completeCallback);
    }

    bool streamGet(const std::string& path,
                   const RequestHeaders& headers,
                   const HttpClientStreamHeadersCallback& headersCallback,
                   const HttpClientStreamDataCallback& dataCallback,
                   const HttpClientStreamCompleteCallback& completeCallback) {
        return sendStreamingRequest("GET", path, headers, "", headersCallback, dataCallback, completeCallback);
    }

    bool streamRequest(const HttpClientRequest& request,
                       const HttpClientStreamHeadersCallback& headersCallback,
                       const HttpClientStreamDataCallback& dataCallback,
                       const HttpClientStreamCompleteCallback& completeCallback) {
        return sendStreamingRequestObject(request, headersCallback, dataCallback, completeCallback);
    }

    void cancelRequest() {
        if (!awaitingResponse_) {
            return;
        }

        setLastError(ErrorCode::ConnectionError, "HTTP request cancelled");
        stopRequestTimer();
        restoreRequestTimeoutOverride();
        awaitingResponse_ = false;
        responseBuffer_.clear();
        tcpClient_.disconnect();

        if (streamingResponse_) {
            completeStreamingWithStatus(0, "HTTP request cancelled");
            return;
        }

        if (responseCallback_) {
            HttpResponse response;
            response.statusCode = 0;
            response.statusMessage = "HTTP request cancelled";
            responseCallback_(response);
        }
    }

    void disconnect() {
        stopRequestTimer();
        tcpClient_.disconnect();
        connected_ = false;
        awaitingResponse_ = false;
        streamingResponse_ = false;
        responseBuffer_.clear();
    }

    void setConnectTimeout(uint32_t timeoutMs) {
        connectTimeout_ = timeoutMs;
        tcpClient_.setConnectTimeout(timeoutMs);
    }

    void setRequestTimeout(uint32_t timeoutMs) {
        requestTimeout_ = timeoutMs;
    }

    void setReadTimeout(uint32_t timeoutMs) {
        readTimeout_ = timeoutMs;
        tcpClient_.setReadTimeout(timeoutMs);
    }

    void setFollowRedirects(bool follow) {
        followRedirects_ = follow;
    }

    void setMaxRedirects(uint32_t maxRedirects) {
        maxRedirects_ = maxRedirects;
    }

    void setUseCompression(bool use) {
        useCompression_ = use;
    }

    void setSSLConfig(const SSLConfig& sslConfig) {
        sslConfig_ = sslConfig;
    }

    bool setProxyUrl(std::string_view proxyUrl) {
        HttpProxyOptions parsedProxy;
        if (!parseProxyUrl(proxyUrl, parsedProxy)) {
            setLastError(ErrorCode::InvalidArgument, "Invalid HTTP proxy URL");
            return false;
        }
        setProxy(parsedProxy);
        return true;
    }

    void setProxy(const HttpProxyOptions& proxyOptions) {
        proxyOptions_ = proxyOptions;
    }

    void clearProxy() {
        proxyOptions_ = {};
    }

    bool isConnected() const {
        return connected_;
    }

    Address getLocalAddress() const {
        return tcpClient_.getLocalAddress();
    }

    Address getRemoteAddress() const {
        return tcpClient_.getRemoteAddress();
    }

    Error getLastError() const {
        std::lock_guard<std::mutex> lock(errorMutex_);
        return lastError_;
    }

    void initializeCallbacks() {
        std::weak_ptr<Impl> weakSelf = shared_from_this();
        tcpClient_.setOwnedDataReceivedCallback([weakSelf](Buffer&& data) {
            auto self = weakSelf.lock();
            if (!self) {
                return;
            }
            self->handleDataReceived(std::move(data));
        });
        tcpClient_.setDisconnectCallback([weakSelf](const std::string& reason) {
            auto self = weakSelf.lock();
            if (!self) {
                return;
            }
            self->handleDisconnect(reason);
        });
        tcpClient_.setErrorCallback([weakSelf](ErrorCode code, const std::string& message) {
            auto self = weakSelf.lock();
            if (!self) {
                return;
            }
            self->handleError(code, message);
        });
    }

private:
    void clearLastError() {
        std::lock_guard<std::mutex> lock(errorMutex_);
        lastError_ = Error::success();
    }

    void setLastError(const Error& error) {
        std::lock_guard<std::mutex> lock(errorMutex_);
        lastError_ = error;
    }

    void setLastError(ErrorCode code, std::string message) {
        setLastError(Error(code, std::move(message)));
    }


    struct ParsedUrl {
        std::string protocol;
        std::string host;
        uint16_t port = 0;
        std::string path = "/";
    };

    struct PendingRequest {
        std::string method;
        std::string path;
        RequestHeaders headers;
        std::string body;
    };

    bool connectToParsedUrl(bool replayRequest) {
        replayAfterConnect_ = replayRequest;
        responseBuffer_.clear();
        connected_ = false;

        if (shouldUseHttpProxy() && parsedUrl_.protocol == "https") {
            const std::string message = "HTTPS proxy tunneling is not supported by HttpClient yet";
            setLastError(ErrorCode::InvalidArgument, message);
            if (connectCallback_) {
                connectCallback_(false, message);
            }
            return false;
        }

        const SSLConfig effectiveSslConfig = buildEffectiveSslConfig();
        const std::string connectHost = shouldUseHttpProxy() ? proxyOptions_.host : parsedUrl_.host;
        const uint16_t connectPort = shouldUseHttpProxy() ? proxyOptions_.port : parsedUrl_.port;

        std::weak_ptr<Impl> weakSelf = shared_from_this();
        const bool started = tcpClient_.connect(
            connectHost,
            connectPort,
            [weakSelf](bool success, const std::string& message) {
                auto self = weakSelf.lock();
                if (!self) {
                    return;
                }
                self->handleConnect(success, message);
            },
            effectiveSslConfig);
        if (!started) {
            const Error transportError = tcpClient_.getLastError();
            setLastError(transportError.isFailure() ? transportError
                                                    : Error(ErrorCode::ConnectionError,
                                                            "Failed to start HTTP connection"));
        }
        return started;
    }

    void handleConnect(bool success, const std::string& errorMessage) {
        connected_ = success;
        if (!success) {
            const Error transportError = tcpClient_.getLastError();
            setLastError(transportError.isFailure() ? transportError
                                                    : Error(ErrorCode::ConnectionError, errorMessage));
            stopRequestTimer();
            awaitingResponse_ = false;
            if (connectCallback_) {
                connectCallback_(false, errorMessage);
            }
            if (replayAfterConnect_ && responseCallback_) {
                HttpResponse response;
                response.statusCode = 0;
                response.statusMessage = errorMessage;
                responseCallback_(response);
            }
            replayAfterConnect_ = false;
            return;
        }

        clearLastError();
        if (replayAfterConnect_) {
            replayAfterConnect_ = false;
            sendPendingRequest();
            return;
        }

        if (connectCallback_) {
            connectCallback_(true, "");
        }
    }

    void handleDataReceived(Buffer data) {
        if (data.empty()) {
            return;
        }

        if (responseBuffer_.empty()) {
            responseBuffer_ = std::move(data);
        } else {
            responseBuffer_.insert(responseBuffer_.end(), data.begin(), data.end());
        }

        if (streamingResponse_) {
            handleStreamingDataReceived(false);
            return;
        }

        HttpResponse response;
        if (!tryConsumeBufferedResponse(false, response)) {
            return;
        }

        awaitingResponse_ = false;
        handleResponse(response);
    }

    void handleDisconnect(const std::string& reason) {
        stopRequestTimer();
        connected_ = false;
        if (awaitingResponse_ && !reason.empty()) {
            setLastError(ErrorCode::ConnectionError, reason);
        }

        if (awaitingResponse_ && streamingResponse_) {
            handleStreamingDataReceived(true);
            if (awaitingResponse_) {
                completeStreamingWithStatus(0, reason.empty() ? "Connection closed" : reason);
            }
            return;
        }

        if (awaitingResponse_ && !responseBuffer_.empty()) {
            HttpResponse response;
            if (tryConsumeBufferedResponse(true, response)) {
                awaitingResponse_ = false;
                handleResponse(response);
                return;
            }
        }

        if (awaitingResponse_ && responseCallback_) {
            HttpResponse response;
            response.statusCode = 0;
            response.statusMessage = reason;
            awaitingResponse_ = false;
            responseBuffer_.clear();
            responseCallback_(response);
        }
    }

    void handleError(ErrorCode code, const std::string& message) {
        setLastError(code, message);
        stopRequestTimer();
        if (streamingResponse_) {
            completeStreamingWithStatus(0, message);
            return;
        }
        restoreRequestTimeoutOverride();
        if (responseCallback_) {
            HttpResponse response;
            response.statusCode = 0;
            response.statusMessage = message;
            awaitingResponse_ = false;
            responseBuffer_.clear();
            responseCallback_(response);
        }
    }

    bool sendRequest(std::string_view method,
                     std::string_view path,
                     const RequestHeaders& headers,
                     std::string_view body,
                     const HttpClientResponseCallback& callback) {
        if (!connected_) {
            setLastError(ErrorCode::ConnectionError, "HTTP client is not connected");
            return false;
        }
        if (awaitingResponse_) {
            setLastError(ErrorCode::AlreadyRunning, "An HTTP request is already in flight");
            return false;
        }
        if (!isValidHttpMethodName(method)) {
            setLastError(ErrorCode::InvalidArgument, "Invalid HTTP method");
            return false;
        }

        clearLastError();
        pendingRequest_.method = std::string(method);
        pendingRequest_.path = buildRequestPath(path);
        pendingRequest_.headers = headers;
        pendingRequest_.body.assign(body.begin(), body.end());
        responseCallback_ = callback;
        return sendPendingRequest();
    }

    bool sendRequestObject(const HttpClientRequest& request, const HttpClientResponseCallback& callback) {
        const uint32_t previousTimeout = requestTimeout_;
        if (request.timeoutMs() != 0) {
            requestTimeout_ = request.timeoutMs();
        }
        const bool result = sendRequest(request.method(), request.path(), request.headers(), request.body(), callback);
        if (!result || request.timeoutMs() == 0) {
            requestTimeout_ = previousTimeout;
        } else {
            pendingRequestOverrideTimeout_ = previousTimeout;
        }
        return result;
    }

    bool sendStreamingRequest(std::string_view method,
                              std::string_view path,
                              const RequestHeaders& headers,
                              std::string_view body,
                              const HttpClientStreamHeadersCallback& headersCallback,
                              const HttpClientStreamDataCallback& dataCallback,
                              const HttpClientStreamCompleteCallback& completeCallback) {
        if (!connected_) {
            setLastError(ErrorCode::ConnectionError, "HTTP client is not connected");
            return false;
        }
        if (awaitingResponse_) {
            setLastError(ErrorCode::AlreadyRunning, "An HTTP request is already in flight");
            return false;
        }
        if (!isValidHttpMethodName(method)) {
            setLastError(ErrorCode::InvalidArgument, "Invalid HTTP method");
            return false;
        }

        clearLastError();
        pendingRequest_.method = std::string(method);
        pendingRequest_.path = buildRequestPath(path);
        pendingRequest_.headers = headers;
        pendingRequest_.body.assign(body.begin(), body.end());
        responseCallback_ = nullptr;
        streamHeadersCallback_ = headersCallback;
        streamDataCallback_ = dataCallback;
        streamCompleteCallback_ = completeCallback;
        streamingResponse_ = true;
        streamHeadersParsed_ = false;
        streamNoBody_ = false;
        streamChunked_ = false;
        streamContentLength_.reset();
        streamReceivedBodyBytes_ = 0;
        streamResponse_ = HttpResponse{};
        return sendPendingRequest();
    }

    bool sendStreamingRequestObject(const HttpClientRequest& request,
                                    const HttpClientStreamHeadersCallback& headersCallback,
                                    const HttpClientStreamDataCallback& dataCallback,
                                    const HttpClientStreamCompleteCallback& completeCallback) {
        const uint32_t previousTimeout = requestTimeout_;
        if (request.timeoutMs() != 0) {
            requestTimeout_ = request.timeoutMs();
        }
        const bool result = sendStreamingRequest(request.method(),
                                                 request.path(),
                                                 request.headers(),
                                                 request.body(),
                                                 headersCallback,
                                                 dataCallback,
                                                 completeCallback);
        if (!result || request.timeoutMs() == 0) {
            requestTimeout_ = previousTimeout;
        } else {
            pendingRequestOverrideTimeout_ = previousTimeout;
        }
        return result;
    }

    bool sendPendingRequest() {
        if (!connected_) {
            setLastError(ErrorCode::ConnectionError, "HTTP client is not connected");
            return false;
        }

        RequestHeaders transportHeaders = pendingRequest_.headers;
        applyProxyHeaders(transportHeaders);

        std::string request = buildClientRequest(pendingRequest_.method,
                                                 buildTransportRequestTarget(pendingRequest_.path),
                                                 transportHeaders,
                                                 pendingRequest_.body,
                                                 buildHostHeader(),
                                                 useCompression_);

        responseBuffer_.clear();
        awaitingResponse_ = true;
        const bool sent = tcpClient_.send(std::move(request));
        if (sent) {
            clearLastError();
            startRequestTimer();
        } else {
            const Error transportError = tcpClient_.getLastError();
            setLastError(transportError.isFailure() ? transportError
                                                    : Error(ErrorCode::ConnectionError,
                                                            "Failed to send HTTP request"));
            awaitingResponse_ = false;
            streamingResponse_ = false;
            restoreRequestTimeoutOverride();
        }
        return sent;
    }

    HttpResponse buildResponse(const HttpResponseView& responseView) const {
        HttpResponse response;
        response.statusCode = responseView.statusCode;
        response.statusMessage = std::string(responseView.statusMessage);
        for (const auto& header : responseView.headers) {
            response.headers[std::string(header.first)] = std::string(header.second);
        }

        auto transferEncoding = responseView.getHeader("Transfer-Encoding");
        if (transferEncoding.has_value() &&
            containsHeaderTokenCaseInsensitive(*transferEncoding, "chunked")) {
            std::string decodedBody;
            if (HttpParser::parseChunked(responseView.body, decodedBody)) {
                response.body = std::move(decodedBody);
                return response;
            }
        }

        response.body.assign(responseView.body.begin(), responseView.body.end());
        return response;
    }

    bool tryConsumeBufferedResponse(bool connectionClosed, HttpResponse& response) {
        while (true) {
            HttpResponseView responseView;
            size_t consumedBytes = 0;
            if (!tryExtractResponseView(connectionClosed, responseView, &consumedBytes)) {
                return false;
            }

            if (consumedBytes >= responseBuffer_.size()) {
                responseBuffer_.clear();
            } else {
                responseBuffer_.erase(responseBuffer_.begin(), responseBuffer_.begin() + consumedBytes);
            }

            if (isInterimResponseStatus(responseView.statusCode)) {
                continue;
            }

            response = buildResponse(responseView);
            return true;
        }
    }

    std::string_view responseBufferView() const {
        return std::string_view(reinterpret_cast<const char*>(responseBuffer_.data()), responseBuffer_.size());
    }

    void eraseResponseBufferPrefix(size_t bytes) {
        if (bytes >= responseBuffer_.size()) {
            responseBuffer_.clear();
            return;
        }
        responseBuffer_.erase(responseBuffer_.begin(), responseBuffer_.begin() + static_cast<std::ptrdiff_t>(bytes));
    }

    bool parseStreamingHeaders(bool connectionClosed) {
        while (!streamHeadersParsed_) {
            const std::string_view raw = responseBufferView();
            const size_t headerEnd = raw.find("\r\n\r\n");
            if (headerEnd == std::string_view::npos) {
                if (connectionClosed) {
                    setLastError(ErrorCode::HttpProtocolError, "Incomplete HTTP response head");
                    completeStreamingWithStatus(0, "Incomplete HTTP response head");
                }
                return false;
            }

            HttpResponseView responseView;
            if (!HttpParser::parseResponseHead(raw, responseView)) {
                setLastError(ErrorCode::HttpProtocolError, "Invalid HTTP response head");
                completeStreamingWithStatus(0, "Invalid HTTP response head");
                return false;
            }

            eraseResponseBufferPrefix(headerEnd + 4);
            if (isInterimResponseStatus(responseView.statusCode)) {
                continue;
            }

            streamResponse_ = buildResponse(responseView);
            streamResponse_.body.clear();
            streamHeadersParsed_ = true;
            streamNoBody_ =
                HttpParser::caseInsensitiveCompare(pendingRequest_.method, "HEAD") ||
                (responseView.statusCode >= 100 && responseView.statusCode < 200) ||
                responseView.statusCode == 204 ||
                responseView.statusCode == 304;

            const auto transferEncoding = responseView.getHeader("Transfer-Encoding");
            streamChunked_ = transferEncoding.has_value() &&
                             containsHeaderTokenCaseInsensitive(*transferEncoding, "chunked");
            streamContentLength_.reset();
            streamReceivedBodyBytes_ = 0;
            const auto contentLength = responseView.getHeader("Content-Length");
            if (!streamChunked_ && contentLength.has_value()) {
                try {
                    streamContentLength_ = static_cast<size_t>(std::stoull(std::string(*contentLength)));
                } catch (...) {
                    setLastError(ErrorCode::HttpProtocolError, "Invalid HTTP Content-Length");
                    completeStreamingWithStatus(0, "Invalid HTTP Content-Length");
                    return false;
                }
            }

            if (streamHeadersCallback_) {
                streamHeadersCallback_(streamResponse_);
                if (!streamingResponse_) {
                    return false;
                }
            }
        }
        return true;
    }

    bool deliverStreamingChunk(std::string_view chunk) {
        if (chunk.empty()) {
            return true;
        }
        if (streamDataCallback_ && !streamDataCallback_(chunk)) {
            setLastError(ErrorCode::ConnectionError, "HTTP stream cancelled by data callback");
            completeStreamingWithStatus(0, "HTTP stream cancelled by data callback");
            tcpClient_.disconnect();
            return false;
        }
        return streamingResponse_;
    }

    bool consumeStreamingChunkedBody() {
        while (streamingResponse_) {
            const std::string_view raw = responseBufferView();
            const size_t lineEnd = raw.find("\r\n");
            if (lineEnd == std::string_view::npos) {
                return true;
            }

            std::string_view sizeLine = HttpParser::trim(raw.substr(0, lineEnd));
            const size_t extensionPos = sizeLine.find(';');
            if (extensionPos != std::string_view::npos) {
                sizeLine = HttpParser::trim(sizeLine.substr(0, extensionPos));
            }

            const auto chunkSize = parseHexSize(sizeLine);
            if (!chunkSize.has_value()) {
                setLastError(ErrorCode::HttpProtocolError, "Invalid HTTP chunk size");
                completeStreamingWithStatus(0, "Invalid HTTP chunk size");
                tcpClient_.disconnect();
                return false;
            }

            const size_t chunkBodyOffset = lineEnd + 2;
            if (*chunkSize == 0) {
                size_t trailerOffset = chunkBodyOffset;
                while (true) {
                    const size_t trailerEnd = raw.find("\r\n", trailerOffset);
                    if (trailerEnd == std::string_view::npos) {
                        return true;
                    }
                    const std::string_view trailerLine = raw.substr(trailerOffset, trailerEnd - trailerOffset);
                    trailerOffset = trailerEnd + 2;
                    if (trailerLine.empty()) {
                        eraseResponseBufferPrefix(trailerOffset);
                        completeStreamingResponse();
                        return false;
                    }
                    const size_t colon = trailerLine.find(':');
                    if (colon == std::string_view::npos || colon == 0) {
                        setLastError(ErrorCode::HttpProtocolError, "Invalid HTTP chunk trailer");
                        completeStreamingWithStatus(0, "Invalid HTTP chunk trailer");
                        tcpClient_.disconnect();
                        return false;
                    }
                }
            }

            if (raw.size() < chunkBodyOffset + *chunkSize + 2) {
                return true;
            }
            if (raw.substr(chunkBodyOffset + *chunkSize, 2) != "\r\n") {
                setLastError(ErrorCode::HttpProtocolError, "Malformed HTTP chunk");
                completeStreamingWithStatus(0, "Malformed HTTP chunk");
                tcpClient_.disconnect();
                return false;
            }

            const std::string_view chunk = raw.substr(chunkBodyOffset, *chunkSize);
            if (!deliverStreamingChunk(chunk)) {
                return false;
            }
            eraseResponseBufferPrefix(chunkBodyOffset + *chunkSize + 2);
        }
        return false;
    }

    bool consumeStreamingFixedLengthBody() {
        if (!streamContentLength_.has_value() || streamReceivedBodyBytes_ >= *streamContentLength_) {
            completeStreamingResponse();
            return false;
        }
        while (streamingResponse_ && streamContentLength_.has_value() && !responseBuffer_.empty()) {
            const size_t remaining = *streamContentLength_ - streamReceivedBodyBytes_;
            if (remaining == 0) {
                completeStreamingResponse();
                return false;
            }
            const size_t bytesToDeliver = std::min(remaining, responseBuffer_.size());
            const std::string_view chunk = responseBufferView().substr(0, bytesToDeliver);
            if (!deliverStreamingChunk(chunk)) {
                return false;
            }
            streamReceivedBodyBytes_ += bytesToDeliver;
            eraseResponseBufferPrefix(bytesToDeliver);
            if (streamReceivedBodyBytes_ >= *streamContentLength_) {
                completeStreamingResponse();
                return false;
            }
        }
        return streamingResponse_;
    }

    void handleStreamingDataReceived(bool connectionClosed) {
        if (!parseStreamingHeaders(connectionClosed) || !streamingResponse_) {
            return;
        }

        if (streamNoBody_) {
            completeStreamingResponse();
            return;
        }

        if (streamChunked_) {
            consumeStreamingChunkedBody();
            return;
        }

        if (streamContentLength_.has_value()) {
            if (*streamContentLength_ == 0 || streamReceivedBodyBytes_ >= *streamContentLength_) {
                completeStreamingResponse();
                return;
            }
            consumeStreamingFixedLengthBody();
            return;
        }

        if (!responseBuffer_.empty()) {
            const std::string_view chunk = responseBufferView();
            if (!deliverStreamingChunk(chunk)) {
                return;
            }
            responseBuffer_.clear();
        }

        if (connectionClosed) {
            completeStreamingResponse();
        }
    }

    void completeStreamingResponse() {
        stopRequestTimer();
        clearLastError();
        restoreRequestTimeoutOverride();
        awaitingResponse_ = false;
        streamingResponse_ = false;
        streamHeadersParsed_ = false;
        streamNoBody_ = false;
        streamChunked_ = false;
        streamContentLength_.reset();
        streamReceivedBodyBytes_ = 0;
        responseBuffer_.clear();
        if (streamCompleteCallback_) {
            streamCompleteCallback_(streamResponse_);
        }
    }

    void completeStreamingWithStatus(int statusCode, const std::string& message) {
        stopRequestTimer();
        restoreRequestTimeoutOverride();
        awaitingResponse_ = false;
        streamingResponse_ = false;
        streamHeadersParsed_ = false;
        streamNoBody_ = false;
        streamChunked_ = false;
        streamContentLength_.reset();
        streamReceivedBodyBytes_ = 0;
        responseBuffer_.clear();

        HttpResponse response = streamResponse_;
        response.statusCode = statusCode;
        response.statusMessage = message;
        response.body.clear();
        if (streamCompleteCallback_) {
            streamCompleteCallback_(response);
        }
    }

    bool tryExtractResponseView(bool connectionClosed, HttpResponseView& responseView, size_t* consumedBytes) const {
        const std::string_view raw(reinterpret_cast<const char*>(responseBuffer_.data()), responseBuffer_.size());
        return tryExtractResponseViewFromRaw(raw, connectionClosed, responseView, consumedBytes);
    }

    bool tryExtractResponseViewFromRaw(std::string_view raw,
                                       bool connectionClosed,
                                       HttpResponseView& responseView,
                                       size_t* consumedBytes) const {
        if (!HttpParser::parseResponseHead(raw, responseView)) {
            return false;
        }

        const size_t headerEnd = raw.find("\r\n\r\n");
        if (headerEnd == std::string_view::npos) {
            return false;
        }
        const size_t headerBytes = headerEnd + 4;

        if (HttpParser::caseInsensitiveCompare(pendingRequest_.method, "HEAD") ||
            (responseView.statusCode >= 100 && responseView.statusCode < 200) ||
            responseView.statusCode == 204 ||
            responseView.statusCode == 304) {
            responseView.body = {};
            if (consumedBytes != nullptr) {
                *consumedBytes = headerBytes;
            }
            return true;
        }

        const auto contentLength = responseView.getHeader("Content-Length");
        if (contentLength.has_value()) {
            size_t length = 0;
            try {
                length = static_cast<size_t>(std::stoull(std::string(*contentLength)));
            } catch (...) {
                return false;
            }
            if (raw.size() < headerBytes + length) {
                return false;
            }
            responseView.body = raw.substr(headerBytes, length);
            if (consumedBytes != nullptr) {
                *consumedBytes = headerBytes + length;
            }
            return true;
        }

        const auto transferEncoding = responseView.getHeader("Transfer-Encoding");
        if (transferEncoding.has_value() &&
            containsHeaderTokenCaseInsensitive(*transferEncoding, "chunked")) {
            const auto chunkedBytes = getChunkedMessageSize(raw.substr(headerBytes));
            if (!chunkedBytes.has_value() || *chunkedBytes == 0) {
                return false;
            }
            responseView.body = raw.substr(headerBytes, *chunkedBytes);
            if (consumedBytes != nullptr) {
                *consumedBytes = headerBytes + *chunkedBytes;
            }
            return true;
        }

        if (!connectionClosed) {
            return false;
        }

        responseView.body = raw.substr(headerBytes);
        if (consumedBytes != nullptr) {
            *consumedBytes = raw.size();
        }
        return true;
    }

    void handleResponse(HttpResponse& response) {
        stopRequestTimer();
        clearLastError();
        if (followRedirects_ &&
            isRedirect(response.statusCode) &&
            redirectCount_ < maxRedirects_) {
            if (const std::string* location = findHeaderCaseInsensitive(response.headers, "Location")) {
                ParsedUrl redirectedUrl;
                if (resolveRedirect(*location, redirectedUrl)) {
                    ++redirectCount_;
                    const bool endpointChanged =
                        redirectedUrl.host != parsedUrl_.host ||
                        redirectedUrl.port != parsedUrl_.port ||
                        redirectedUrl.protocol != parsedUrl_.protocol;

                    prepareRedirectRequest(response.statusCode, redirectedUrl, endpointChanged);
                    parsedUrl_ = redirectedUrl;
                    if (endpointChanged) {
                        disconnect();
                        connectToParsedUrl(true);
                        return;
                    }

                    sendPendingRequest();
                    return;
                }
            }
        }

        restoreRequestTimeoutOverride();
        if (responseCallback_) {
            responseCallback_(response);
        }
    }

    void startRequestTimer() {
        stopRequestTimer();
        if (!requestTimer_ || requestTimeout_ == 0) {
            return;
        }

        std::weak_ptr<Impl> weakSelf = shared_from_this();
        requestTimer_->start(std::chrono::milliseconds(requestTimeout_), [weakSelf]() {
            auto self = weakSelf.lock();
            if (!self || !self->awaitingResponse_) {
                return;
            }

            self->awaitingResponse_ = false;
            self->responseBuffer_.clear();
            self->setLastError(ErrorCode::TimeoutError, "Request timeout");
            self->tcpClient_.disconnect();

            if (self->streamingResponse_) {
                self->completeStreamingWithStatus(0, "Request timeout");
                return;
            }

            self->restoreRequestTimeoutOverride();
            if (self->responseCallback_) {
                HttpResponse response;
                response.statusCode = 0;
                response.statusMessage = "Request timeout";
                self->responseCallback_(response);
            }
        });
    }

    void stopRequestTimer() {
        if (requestTimer_) {
            requestTimer_->stop();
        }
    }

    void restoreRequestTimeoutOverride() {
        if (!pendingRequestOverrideTimeout_.has_value()) {
            return;
        }
        requestTimeout_ = *pendingRequestOverrideTimeout_;
        pendingRequestOverrideTimeout_.reset();
    }

    SSLConfig buildEffectiveSslConfig() const {
        if (shouldUseHttpProxy()) {
            return SSLConfig{};
        }
        SSLConfig effective = sslConfig_;
        effective.enableSSL = parsedUrl_.protocol == "https";
        if (effective.enableSSL &&
            effective.hostnameVerification.empty() &&
            !Address::isValidIPv4(parsedUrl_.host) &&
            !Address::isValidIPv6(parsedUrl_.host)) {
            effective.hostnameVerification = parsedUrl_.host;
        }
        return effective;
    }

    bool shouldUseHttpProxy() const {
        return proxyOptions_.enabled();
    }

    std::string buildTransportRequestTarget(const std::string& path) const {
        if (!shouldUseHttpProxy() || parsedUrl_.protocol != "http" || path == "*") {
            return path;
        }
        return parsedUrl_.protocol + "://" + buildHostHeader() + path;
    }

    void applyProxyHeaders(RequestHeaders& headers) const {
        if (!shouldUseHttpProxy()) {
            return;
        }

        for (const auto& [name, value] : proxyOptions_.headers) {
            headers[name] = value;
        }

        if (!proxyOptions_.username.empty()) {
            eraseHeaderCaseInsensitive(headers, "Proxy-Authorization");
            headers["Proxy-Authorization"] =
                "Basic " + base64Encode(proxyOptions_.username + ":" + proxyOptions_.password);
        }
    }

    std::string buildHostHeader() const {
        const bool defaultPort =
            (parsedUrl_.protocol == "http" && parsedUrl_.port == 80) ||
            (parsedUrl_.protocol == "https" && parsedUrl_.port == 443);
        const std::string renderedHost =
            Address::isValidIPv6(parsedUrl_.host) ? ("[" + parsedUrl_.host + "]") : parsedUrl_.host;
        if (defaultPort) {
            return renderedHost;
        }
        return renderedHost + ":" + std::to_string(parsedUrl_.port);
    }

    bool parseProxyUrl(std::string_view proxyUrl, HttpProxyOptions& result) const {
        result = {};
        proxyUrl = HttpParser::trim(proxyUrl);
        if (proxyUrl.empty()) {
            return false;
        }

        std::string protocol = "http";
        const size_t protocolPos = proxyUrl.find("://");
        if (protocolPos != std::string_view::npos) {
            protocol = toLowerCopy(proxyUrl.substr(0, protocolPos));
            proxyUrl.remove_prefix(protocolPos + 3);
        }
        if (protocol != "http") {
            return false;
        }

        const size_t pathPos = proxyUrl.find_first_of("/?#");
        std::string_view authority =
            pathPos == std::string_view::npos ? proxyUrl : proxyUrl.substr(0, pathPos);
        if (authority.empty()) {
            return false;
        }

        const size_t userInfoPos = authority.rfind('@');
        if (userInfoPos != std::string_view::npos) {
            const std::string_view userInfo = authority.substr(0, userInfoPos);
            authority.remove_prefix(userInfoPos + 1);
            const size_t colonPos = userInfo.find(':');
            if (colonPos == std::string_view::npos) {
                result.username = HttpParser::urlDecode(userInfo);
            } else {
                result.username = HttpParser::urlDecode(userInfo.substr(0, colonPos));
                result.password = HttpParser::urlDecode(userInfo.substr(colonPos + 1));
            }
        }

        const auto address = Address::parse(authority, 8080);
        if (!address.has_value() || address->port == 0) {
            return false;
        }
        result.host = address->normalizedHost();
        result.port = address->port;
        return true;
    }

    bool parseUrl(std::string_view url, ParsedUrl& result) const {
        result = ParsedUrl{};
        url = HttpParser::trim(url);

        const size_t protocolPos = url.find("://");
        if (protocolPos == std::string::npos) {
            return false;
        }

        result.protocol = toLowerCopy(url.substr(0, protocolPos));
        if (result.protocol != "http" && result.protocol != "https") {
            return false;
        }

        const size_t authorityStart = protocolPos + 3;
        const size_t pathPos = url.find_first_of("/?#", authorityStart);
        std::string_view authority =
            pathPos == std::string_view::npos ? url.substr(authorityStart) : url.substr(authorityStart, pathPos - authorityStart);
        if (authority.empty()) {
            return false;
        }

        const size_t userInfoPos = authority.rfind('@');
        if (userInfoPos != std::string_view::npos) {
            authority.remove_prefix(userInfoPos + 1);
        }

        const uint16_t defaultPort = result.protocol == "https" ? 443 : 80;
        const auto address = Address::parse(authority, defaultPort);
        if (!address.has_value() || address->port == 0) {
            return false;
        }

        result.host = address->normalizedHost();
        result.port = address->port;

        std::string_view target =
            pathPos == std::string_view::npos ? std::string_view("/") : url.substr(pathPos);
        const size_t fragmentPos = target.find('#');
        if (fragmentPos != std::string_view::npos) {
            target = target.substr(0, fragmentPos);
        }
        if (target.empty()) {
            target = "/";
        }
        if (target.front() == '?') {
            result.path = "/" + std::string(target);
        } else {
            result.path = buildRequestPath(target);
        }

        return !result.host.empty() && !result.path.empty();
    }

    bool resolveRedirect(std::string_view location, ParsedUrl& redirectedUrl) const {
        location = HttpParser::trim(location);
        const size_t fragmentPos = location.find('#');
        if (fragmentPos != std::string_view::npos) {
            location = location.substr(0, fragmentPos);
        }
        if (location.empty()) {
            redirectedUrl = parsedUrl_;
            return true;
        }

        if (location.find("://") != std::string_view::npos) {
            return parseUrl(location, redirectedUrl);
        }
        if (location.rfind("//", 0) == 0) {
            return parseUrl(parsedUrl_.protocol + ":" + std::string(location), redirectedUrl);
        }

        redirectedUrl = parsedUrl_;
        if (location.front() == '/') {
            redirectedUrl.path = normalizeHttpPath(location);
            return true;
        }
        if (location.front() == '?') {
            const size_t queryPos = parsedUrl_.path.find('?');
            const std::string_view basePath =
                queryPos == std::string::npos ? std::string_view(parsedUrl_.path) : std::string_view(parsedUrl_.path).substr(0, queryPos);
            redirectedUrl.path = std::string(basePath.empty() ? std::string_view("/") : basePath) + std::string(location);
            return true;
        }
        redirectedUrl.path = joinRelativePath(parsedUrl_.path, location);
        return true;
    }

    static bool isRedirect(int statusCode) {
        return statusCode == 301 || statusCode == 302 || statusCode == 303 ||
               statusCode == 307 || statusCode == 308;
    }

    std::string buildRequestPath(std::string_view path) const {
        if (path.empty()) {
            return parsedUrl_.path.empty() ? "/" : parsedUrl_.path;
        }
        if (path == "*") {
            return "*";
        }
        const size_t fragmentPos = path.find('#');
        if (fragmentPos != std::string_view::npos) {
            path = path.substr(0, fragmentPos);
        }
        if (path.empty()) {
            return parsedUrl_.path.empty() ? "/" : parsedUrl_.path;
        }
        if (path.front() == '?') {
            const size_t queryPos = parsedUrl_.path.find('?');
            const std::string_view basePath =
                queryPos == std::string::npos ? std::string_view(parsedUrl_.path) : std::string_view(parsedUrl_.path).substr(0, queryPos);
            return std::string(basePath.empty() ? std::string_view("/") : basePath) + std::string(path);
        }
        if (path.find("://") != std::string_view::npos) {
            ParsedUrl absoluteUrl;
            if (parseUrl(path, absoluteUrl)) {
                return absoluteUrl.path;
            }
        }
        if (path.front() == '/') {
            return normalizeHttpPath(path);
        }
        return normalizeHttpPath("/" + std::string(path));
    }

    void prepareRedirectRequest(int statusCode, const ParsedUrl& redirectedUrl, bool endpointChanged) {
        const bool switchToGet =
            statusCode == 303 ||
            ((statusCode == 301 || statusCode == 302) &&
             !HttpParser::caseInsensitiveCompare(pendingRequest_.method, "GET") &&
             !HttpParser::caseInsensitiveCompare(pendingRequest_.method, "HEAD"));
        if (switchToGet) {
            pendingRequest_.method = "GET";
            pendingRequest_.body.clear();
            eraseHeaderCaseInsensitive(pendingRequest_.headers, "Content-Length");
            eraseHeaderCaseInsensitive(pendingRequest_.headers, "Content-Type");
            eraseHeaderCaseInsensitive(pendingRequest_.headers, "Transfer-Encoding");
        }

        pendingRequest_.path = buildRequestPath(redirectedUrl.path);

        if (endpointChanged) {
            eraseHeaderCaseInsensitive(pendingRequest_.headers, "Host");
            eraseHeaderCaseInsensitive(pendingRequest_.headers, "Authorization");
            eraseHeaderCaseInsensitive(pendingRequest_.headers, "Proxy-Authorization");
        }
    }

    IoService& ioService_;
    TcpClient tcpClient_;
    bool connected_ = false;
    bool awaitingResponse_ = false;
    bool replayAfterConnect_ = false;
    bool streamingResponse_ = false;
    bool streamHeadersParsed_ = false;
    bool streamNoBody_ = false;
    bool streamChunked_ = false;
    std::optional<size_t> streamContentLength_;
    size_t streamReceivedBodyBytes_ = 0;

    ParsedUrl parsedUrl_;
    PendingRequest pendingRequest_;
    Buffer responseBuffer_;
    HttpResponse streamResponse_;
    std::unique_ptr<Timer> requestTimer_;

    uint32_t connectTimeout_ = 5000;
    uint32_t requestTimeout_ = 5000;
    uint32_t readTimeout_ = 5000;
    std::optional<uint32_t> pendingRequestOverrideTimeout_;
    bool followRedirects_ = true;
    uint32_t maxRedirects_ = 5;
    uint32_t redirectCount_ = 0;
    bool useCompression_ = false;
    SSLConfig sslConfig_;
    HttpProxyOptions proxyOptions_;
    mutable std::mutex errorMutex_;
    Error lastError_;

    HttpClientConnectCallback connectCallback_;
    HttpClientResponseCallback responseCallback_;
    HttpClientStreamHeadersCallback streamHeadersCallback_;
    HttpClientStreamDataCallback streamDataCallback_;
    HttpClientStreamCompleteCallback streamCompleteCallback_;
};

HttpClient::HttpClient(IoService& ioService)
    : impl_(std::make_shared<Impl>(ioService)) {
    impl_->initializeCallbacks();
}

HttpClient::~HttpClient() = default;

bool HttpClient::connect(const std::string& url, const HttpClientConnectCallback& callback) {
    return impl_->connect(url, callback);
}

bool HttpClient::get(const std::string& path, const RequestHeaders& headers, const HttpClientResponseCallback& callback) {
    return impl_->get(path, headers, callback);
}

bool HttpClient::head(const std::string& path, const RequestHeaders& headers, const HttpClientResponseCallback& callback) {
    return impl_->head(path, headers, callback);
}

bool HttpClient::post(const std::string& path,
                      const RequestHeaders& headers,
                      const std::string& body,
                      const HttpClientResponseCallback& callback) {
    return impl_->post(path, headers, body, callback);
}

bool HttpClient::put(const std::string& path,
                     const RequestHeaders& headers,
                     const std::string& body,
                     const HttpClientResponseCallback& callback) {
    return impl_->put(path, headers, body, callback);
}

bool HttpClient::patch(const std::string& path,
                       const RequestHeaders& headers,
                       const std::string& body,
                       const HttpClientResponseCallback& callback) {
    return impl_->patch(path, headers, body, callback);
}

bool HttpClient::del(const std::string& path, const RequestHeaders& headers, const HttpClientResponseCallback& callback) {
    return impl_->del(path, headers, callback);
}

bool HttpClient::request(std::string_view method,
                         std::string_view path,
                         const RequestHeaders& headers,
                         std::string_view body,
                         const HttpClientResponseCallback& callback) {
    return impl_->request(method, path, headers, body, callback);
}

bool HttpClient::request(const HttpClientRequest& request, const HttpClientResponseCallback& callback) {
    return impl_->request(request, callback);
}

bool HttpClient::streamRequest(std::string_view method,
                               std::string_view path,
                               const RequestHeaders& headers,
                               std::string_view body,
                               const HttpClientStreamHeadersCallback& headersCallback,
                               const HttpClientStreamDataCallback& dataCallback,
                               const HttpClientStreamCompleteCallback& completeCallback) {
    return impl_->streamRequest(method, path, headers, body, headersCallback, dataCallback, completeCallback);
}

bool HttpClient::streamRequest(const HttpClientRequest& request,
                               const HttpClientStreamHeadersCallback& headersCallback,
                               const HttpClientStreamDataCallback& dataCallback,
                               const HttpClientStreamCompleteCallback& completeCallback) {
    return impl_->streamRequest(request, headersCallback, dataCallback, completeCallback);
}

bool HttpClient::streamGet(const std::string& path,
                           const RequestHeaders& headers,
                           const HttpClientStreamHeadersCallback& headersCallback,
                           const HttpClientStreamDataCallback& dataCallback,
                           const HttpClientStreamCompleteCallback& completeCallback) {
    return impl_->streamGet(path, headers, headersCallback, dataCallback, completeCallback);
}

void HttpClient::cancelRequest() {
    impl_->cancelRequest();
}

void HttpClient::disconnect() {
    impl_->disconnect();
}

void HttpClient::setConnectTimeout(uint32_t timeoutMs) {
    impl_->setConnectTimeout(timeoutMs);
}

void HttpClient::setRequestTimeout(uint32_t timeoutMs) {
    impl_->setRequestTimeout(timeoutMs);
}

void HttpClient::setReadTimeout(uint32_t timeoutMs) {
    impl_->setReadTimeout(timeoutMs);
}

void HttpClient::setFollowRedirects(bool follow) {
    impl_->setFollowRedirects(follow);
}

void HttpClient::setMaxRedirects(uint32_t maxRedirects) {
    impl_->setMaxRedirects(maxRedirects);
}

void HttpClient::setUseCompression(bool use) {
    impl_->setUseCompression(use);
}

void HttpClient::setSSLConfig(const SSLConfig& sslConfig) {
    impl_->setSSLConfig(sslConfig);
}

bool HttpClient::setProxyUrl(std::string_view proxyUrl) {
    return impl_->setProxyUrl(proxyUrl);
}

void HttpClient::setProxy(const HttpProxyOptions& proxyOptions) {
    impl_->setProxy(proxyOptions);
}

void HttpClient::clearProxy() {
    impl_->clearProxy();
}

bool HttpClient::isConnected() const {
    return impl_->isConnected();
}

Address HttpClient::getLocalAddress() const {
    return impl_->getLocalAddress();
}

Address HttpClient::getRemoteAddress() const {
    return impl_->getRemoteAddress();
}

Error HttpClient::getLastError() const {
    return impl_->getLastError();
}

} // namespace FastNet
