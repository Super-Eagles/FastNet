/**
 * @file HttpServer.cpp
 * @brief FastNet HTTP server implementation
 */
#include "HttpServer.h"

#include "HttpParser.h"
#include "TcpServer.h"
#include "Timer.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string_view>
#include <ctime>
#include <unordered_map>
#include <utility>

namespace FastNet {

std::optional<std::string> HttpRequest::getHeader(std::string_view name) const {
    for (const auto& [key, value] : headers) {
        if (HttpParser::caseInsensitiveCompare(key, name)) {
            return value;
        }
    }
    return std::nullopt;
}

class HttpServer::Impl : public std::enable_shared_from_this<HttpServer::Impl> {
public:
    explicit Impl(IoService& ioService)
        : ioService_(ioService),
          tcpServer_(ioService) {}

    ~Impl() {
        stop();
    }

    Error start(uint16_t port, const std::string& bindAddress, const SSLConfig& sslConfig) {
        sslConfig_ = sslConfig;
        Error result = tcpServer_.start(port, bindAddress, sslConfig);
        if (result.isFailure()) {
            return result;
        }

        running_ = true;
        return FASTNET_SUCCESS;
    }

    void stop() {
        if (!running_) {
            return;
        }

        running_ = false;
        tcpServer_.stop();

        std::lock_guard<std::mutex> lock(clientsMutex_);
        clients_.clear();
        std::lock_guard<std::mutex> cacheLock(staticFileCacheMutex_);
        staticFileCache_.clear();
    }

    void registerHandler(const std::string& path, HttpMethod method, const RequestHandler& handler) {
        std::lock_guard<std::mutex> lock(handlersMutex_);
        handlers_[RouteKey{method, normalizePath(path)}] = handler;
    }

    void registerHandler(std::string_view path, HttpMethod method, const RequestHandler& handler) {
        registerHandler(std::string(path), method, handler);
    }

    void registerStaticFileHandler(const std::string& pathPrefix, const std::string& directory) {
        {
            std::lock_guard<std::mutex> lock(handlersMutex_);
            staticFileHandlers_[normalizePath(pathPrefix)] = std::filesystem::path(directory);
        }
        std::lock_guard<std::mutex> cacheLock(staticFileCacheMutex_);
        staticFileCache_.clear();
    }

    void registerStaticFileHandler(std::string_view pathPrefix, std::string_view directory) {
        registerStaticFileHandler(std::string(pathPrefix), std::string(directory));
    }

    void setRequestHandler(const RequestHandler& handler) {
        std::lock_guard<std::mutex> lock(handlersMutex_);
        defaultHandler_ = handler;
    }

    void setConnectionTimeout(uint32_t timeoutMs) {
        connectionTimeout_ = timeoutMs;
        tcpServer_.setConnectionTimeout(timeoutMs);
    }

    void setRequestTimeout(uint32_t timeoutMs) {
        requestTimeout_ = timeoutMs;
    }

    void setWriteTimeout(uint32_t timeoutMs) {
        writeTimeout_ = timeoutMs;
        tcpServer_.setWriteTimeout(timeoutMs);
    }

    void setMaxConnections(size_t maxConnections) {
        maxConnections_ = maxConnections;
        tcpServer_.setMaxConnections(maxConnections);
    }

    void setMaxRequestSize(size_t bytes) {
        maxRequestSize_ = std::max<size_t>(1024, bytes);
    }

    void setStaticFileCacheLimit(size_t bytes) {
        staticFileCacheLimit_ = bytes;
        std::lock_guard<std::mutex> lock(staticFileCacheMutex_);
        staticFileCache_.clear();
    }

    void setSSLConfig(const SSLConfig& sslConfig) {
        sslConfig_ = sslConfig;
    }

    void setServerErrorCallback(const HttpServerErrorCallback& callback) {
        serverErrorCallback_ = callback;
    }

    size_t getClientCount() const { return tcpServer_.getClientCount(); }
    std::vector<ConnectionId> getClientIds() const { return tcpServer_.getClientIds(); }
    Address getListenAddress() const { return tcpServer_.getListenAddress(); }
    bool isRunning() const { return running_; }

    void initializeCallbacks() {
        std::weak_ptr<Impl> weakSelf = shared_from_this();
        tcpServer_.setClientConnectedCallback([weakSelf](ConnectionId clientId, const Address& address) {
            auto self = weakSelf.lock();
            if (!self) {
                return;
            }
            self->handleClientConnected(clientId, address);
        });
        tcpServer_.setClientDisconnectedCallback([weakSelf](ConnectionId clientId, const std::string&) {
            auto self = weakSelf.lock();
            if (!self) {
                return;
            }
            self->handleClientDisconnected(clientId);
        });
        tcpServer_.setOwnedDataReceivedCallback(
            [weakSelf](ConnectionId clientId, Buffer&& data) {
            auto self = weakSelf.lock();
            if (!self) {
                return;
            }
            self->handleDataReceived(clientId, std::move(data));
        });
        tcpServer_.setServerErrorCallback([weakSelf](const Error& error) {
            auto self = weakSelf.lock();
            if (!self) {
                return;
            }
            self->reportError(error);
        });
    }

private:
    struct ClientState {
        explicit ClientState(IoService& ioService)
            : requestTimer(std::make_unique<Timer>(ioService)) {}

        Buffer buffer;
        size_t bufferOffset = 0;
        Address clientAddress;
        bool processing = false;
        std::unique_ptr<Timer> requestTimer;
    };

    struct ParsedRequest {
        HttpRequest request;
        size_t consumedBytes = 0;
        bool keepAlive = true;
        std::optional<HttpResponse> immediateResponse;
    };

    struct StaticFileCacheEntry {
        uintmax_t size = 0;
        std::filesystem::file_time_type lastWriteTime{};
        std::string contentType;
        std::string etag;
        std::string lastModified;
        std::string body;
    };

    struct StaticFileHandlerBinding {
        std::string prefix;
        std::filesystem::path directory;
    };

    struct RouteKey {
        HttpMethod method = HttpMethod::GET;
        std::string path;

        bool operator==(const RouteKey& other) const noexcept {
            return method == other.method && path == other.path;
        }
    };

    struct RouteKeyHash {
        size_t operator()(const RouteKey& key) const noexcept {
            const size_t methodHash = std::hash<int>{}(static_cast<int>(key.method));
            const size_t pathHash = std::hash<std::string>{}(key.path);
            return methodHash ^ (pathHash + 0x9e3779b97f4a7c15ULL + (methodHash << 6) + (methodHash >> 2));
        }
    };

    void handleClientConnected(ConnectionId clientId, const Address& address) {
        std::lock_guard<std::mutex> lock(clientsMutex_);
        auto state = std::make_unique<ClientState>(ioService_);
        state->clientAddress = address;
        clients_[clientId] = std::move(state);
    }

    void handleClientDisconnected(ConnectionId clientId) {
        std::lock_guard<std::mutex> lock(clientsMutex_);
        clients_.erase(clientId);
    }

    void handleDataReceived(ConnectionId clientId, Buffer data) {
        bool shouldProcess = false;
        {
            std::lock_guard<std::mutex> lock(clientsMutex_);
            auto it = clients_.find(clientId);
            if (it == clients_.end()) {
                return;
            }

            auto& state = *it->second;
            compactClientBufferLocked(state);
            if (state.bufferOffset == 0 && state.buffer.empty()) {
                state.buffer = std::move(data);
            } else {
                state.buffer.insert(state.buffer.end(), data.begin(), data.end());
            }
            armRequestTimer(clientId, state);

            if (!state.processing) {
                state.processing = true;
                shouldProcess = true;
            }
        }

        if (shouldProcess) {
            processBufferedRequests(clientId);
        }
    }

    void processBufferedRequests(ConnectionId clientId) {
        while (true) {
            ParsedRequest parsedRequest;
            bool hasRequest = false;

            {
                std::lock_guard<std::mutex> lock(clientsMutex_);
                auto it = clients_.find(clientId);
                if (it == clients_.end()) {
                    return;
                }

                auto& state = *it->second;
                const std::string_view pending = makeBufferedView(state);
                if (pending.size() > maxRequestSize_) {
                    parsedRequest.consumedBytes = pending.size();
                    parsedRequest.keepAlive = false;
                    parsedRequest.immediateResponse = makeErrorResponse(
                        413, "Payload Too Large", "HTTP request too large");
                    state.processing = false;
                    state.buffer.clear();
                    state.bufferOffset = 0;
                    stopRequestTimer(state);
                    hasRequest = true;
                } else {
                    auto result = tryExtractRequest(state);
                    if (result.has_value()) {
                        parsedRequest = std::move(*result);
                        hasRequest = true;
                        state.bufferOffset += parsedRequest.consumedBytes;
                        compactClientBufferLocked(state);
                        if (makeBufferedView(state).empty()) {
                            stopRequestTimer(state);
                        } else {
                            armRequestTimer(clientId, state);
                        }
                    } else {
                        state.processing = false;
                        return;
                    }
                }
            }

            if (!hasRequest) {
                return;
            }

            HttpResponse response =
                parsedRequest.immediateResponse.has_value()
                    ? *parsedRequest.immediateResponse
                    : dispatchRequest(parsedRequest.request);
            finalizeResponse(parsedRequest.request, parsedRequest.keepAlive, response);

            Error sendResult = FASTNET_SUCCESS;
            if (response.hasFileBody) {
                std::string responseHead =
                    HttpParser::buildResponse(response.statusCode, response.statusMessage, response.headers, "");
                sendResult = tcpServer_.sendFileToClient(clientId,
                                                         std::move(responseHead),
                                                         response.filePath,
                                                         response.fileOffset,
                                                         response.fileLength);
            } else {
                std::string responseText =
                    HttpParser::buildResponse(response.statusCode, response.statusMessage, response.headers, response.body);
                sendResult = tcpServer_.sendToClient(clientId, std::move(responseText));
            }
            if (sendResult.isFailure()) {
                reportError(sendResult);
                tcpServer_.disconnectClient(clientId);
                return;
            }

            if (!parsedRequest.keepAlive) {
                tcpServer_.closeClientAfterPendingWrites(clientId);
                return;
            }
        }
    }

    std::optional<ParsedRequest> tryExtractRequest(ClientState& state) {
        const std::string_view raw = makeBufferedView(state);
        const size_t headerEnd = raw.find("\r\n\r\n");
        if (headerEnd == std::string_view::npos) {
            return std::nullopt;
        }

        const size_t headerBytes = headerEnd + 4;
        const std::string_view headerView = raw.substr(0, headerBytes);

        HttpRequestView requestView;
        if (!HttpParser::parseRequestHead(headerView, requestView)) {
            reportError(FASTNET_ERROR(ErrorCode::HttpProtocolError, "Invalid HTTP request"));
            return makeErrorRequest(raw.size(), 400, "Bad Request", "Invalid HTTP request");
        }

        size_t totalBytes = headerBytes;
        std::string decodedBody;

        const auto transferEncoding = requestView.getHeader("Transfer-Encoding");
        if (transferEncoding.has_value() &&
            containsToken(*transferEncoding, "chunked")) {
            const auto chunkedSize = getChunkedMessageSize(raw.substr(headerBytes));
            if (!chunkedSize.has_value()) {
                return std::nullopt;
            }
            if (*chunkedSize == 0) {
                reportError(FASTNET_ERROR(ErrorCode::HttpProtocolError, "Malformed chunked HTTP request"));
                return makeErrorRequest(raw.size(), 400, "Bad Request", "Malformed chunked body");
            }
            totalBytes += *chunkedSize;
        } else {
            const auto contentLength = requestView.getHeader("Content-Length");
            if (contentLength.has_value()) {
                size_t length = 0;
                try {
                    length = static_cast<size_t>(std::stoull(std::string(*contentLength)));
                } catch (...) {
                    reportError(FASTNET_ERROR(ErrorCode::HttpProtocolError, "Invalid Content-Length"));
                    return makeErrorRequest(raw.size(), 400, "Bad Request", "Invalid Content-Length");
                }

                totalBytes += length;
                if (raw.size() < totalBytes) {
                    return std::nullopt;
                }
            }
        }

        if (raw.size() < totalBytes) {
            return std::nullopt;
        }

        requestView.body = raw.substr(headerBytes, totalBytes - headerBytes);

        const auto parsedMethod = tryStringToMethod(requestView.method);
        if (!parsedMethod.has_value()) {
            reportError(FASTNET_ERROR(ErrorCode::HttpProtocolError, "Unsupported HTTP method"));
            return makeErrorRequest(totalBytes, 501, "Not Implemented", "Unsupported HTTP method");
        }

        ParsedRequest result;
        result.consumedBytes = totalBytes;
        result.request = buildRequest(requestView, *parsedMethod, state.clientAddress, decodedBody);
        result.keepAlive = shouldKeepAlive(requestView);
        return result;
    }

    ParsedRequest makeErrorRequest(size_t consumedBytes,
                                   int statusCode,
                                   const std::string& statusMessage,
                                   const std::string& body) const {
        ParsedRequest request;
        request.consumedBytes = consumedBytes;
        request.keepAlive = false;
        request.request.method = HttpMethod::GET;
        request.request.path = "/";
        request.request.version = "HTTP/1.1";
        request.request.headers.clear();
        request.request.body = body;
        request.immediateResponse = makeErrorResponse(statusCode, statusMessage, body);
        return request;
    }

    static HttpResponse makeErrorResponse(int statusCode,
                                          const std::string& statusMessage,
                                          const std::string& body) {
        HttpResponse response;
        response.statusCode = statusCode;
        response.statusMessage = statusMessage;
        response.body = body;
        response.headers["Content-Type"] = "text/plain; charset=utf-8";
        return response;
    }

    HttpRequest buildRequest(const HttpRequestView& view,
                             HttpMethod method,
                             const Address& clientAddress,
                             std::string& decodedChunkedBody) const {
        HttpRequest request;
        request.method = method;
        request.methodName = std::string(view.method);
        request.target = std::string(view.target);
        request.path = normalizeRequestPath(view.uri);
        request.queryString = std::string(view.queryString);
        request.version = std::string(view.version);
        request.clientAddress = clientAddress;

        for (const auto& [key, value] : view.queryParams) {
            request.queryParams[HttpParser::urlDecode(key)] = HttpParser::urlDecode(value);
        }

        for (const auto& [key, value] : view.headers) {
            request.headers[std::string(key)] = std::string(value);
        }

        const auto transferEncoding = view.getHeader("Transfer-Encoding");
        if (transferEncoding.has_value() && containsToken(*transferEncoding, "chunked")) {
            if (HttpParser::parseChunked(view.body, decodedChunkedBody)) {
                request.body = decodedChunkedBody;
            } else {
                request.body.assign(view.body.begin(), view.body.end());
            }
        } else {
            request.body.assign(view.body.begin(), view.body.end());
        }

        return request;
    }

    HttpResponse dispatchRequest(const HttpRequest& request) {
        HttpResponse response;
        response.statusCode = 200;
        response.statusMessage = "OK";
        response.headers["Server"] = "FastNet-HttpServer/2.0";

        RequestHandler exactHandler;
        RequestHandler defaultHandler;
        std::optional<StaticFileHandlerBinding> staticFileHandler;
        size_t bestStaticPrefixLength = 0;
        const std::string normalizedPath = normalizePath(request.path);
        {
            std::lock_guard<std::mutex> lock(handlersMutex_);
            auto it = handlers_.find(RouteKey{request.method, normalizedPath});
            if (it != handlers_.end()) {
                exactHandler = it->second;
            }
            defaultHandler = defaultHandler_;
            for (const auto& [prefix, directory] : staticFileHandlers_) {
                if (normalizedPath.rfind(prefix, 0) != 0) {
                    continue;
                }
                if (prefix != "/" &&
                    normalizedPath != prefix &&
                    normalizedPath.rfind(prefix + "/", 0) != 0) {
                    continue;
                }
                if (!staticFileHandler.has_value() || prefix.size() > bestStaticPrefixLength) {
                    staticFileHandler = StaticFileHandlerBinding{prefix, directory};
                    bestStaticPrefixLength = prefix.size();
                }
            }
        }

        if (exactHandler) {
            exactHandler(request, response);
            return response;
        }

        if (handleStaticFileRequest(request, staticFileHandler, response)) {
            return response;
        }

        if (defaultHandler) {
            defaultHandler(request, response);
            return response;
        }

        response.statusCode = 404;
        response.statusMessage = "Not Found";
        response.body = "404 Not Found";
        response.headers["Content-Type"] = "text/plain; charset=utf-8";
        return response;
    }

    bool handleStaticFileRequest(const HttpRequest& request,
                                 const std::optional<StaticFileHandlerBinding>& staticFileHandler,
                                 HttpResponse& response) const {
        if (!staticFileHandler.has_value()) {
            return false;
        }
        const std::string normalizedPath = normalizePath(request.path);
        const std::string& prefix = staticFileHandler->prefix;
        const std::filesystem::path& directory = staticFileHandler->directory;

        std::string relativePath = normalizedPath.substr(prefix.size());
        if (relativePath.empty() || relativePath == "/") {
            relativePath = "/index.html";
        }
        if (relativePath.front() != '/') {
            relativePath.insert(relativePath.begin(), '/');
        }

        const std::filesystem::path requestedPath = directory / relativePath.substr(1);
        std::error_code error;
        const std::filesystem::path canonicalBase = std::filesystem::weakly_canonical(directory, error);
        if (error) {
            response.statusCode = 500;
            response.statusMessage = "Internal Server Error";
            response.body = "Static file root is invalid";
            response.headers["Content-Type"] = "text/plain; charset=utf-8";
            return true;
        }

        const std::filesystem::path canonicalPath = std::filesystem::weakly_canonical(requestedPath, error);
        if (error || canonicalPath.empty()) {
            response.statusCode = 404;
            response.statusMessage = "Not Found";
            response.body = "404 Not Found";
            response.headers["Content-Type"] = "text/plain; charset=utf-8";
            return true;
        }

        auto baseIt = canonicalBase.begin();
        auto pathIt = canonicalPath.begin();
        while (baseIt != canonicalBase.end() &&
               pathIt != canonicalPath.end() &&
               *baseIt == *pathIt) {
            ++baseIt;
            ++pathIt;
        }
        if (baseIt != canonicalBase.end()) {
            response.statusCode = 403;
            response.statusMessage = "Forbidden";
            response.body = "403 Forbidden";
            response.headers["Content-Type"] = "text/plain; charset=utf-8";
            return true;
        }

        if (!std::filesystem::exists(canonicalPath) || !std::filesystem::is_regular_file(canonicalPath)) {
            response.statusCode = 404;
            response.statusMessage = "Not Found";
            response.body = "404 Not Found";
            response.headers["Content-Type"] = "text/plain; charset=utf-8";
            return true;
        }

        const auto lastWriteTime = std::filesystem::last_write_time(canonicalPath, error);
        if (error) {
            response.statusCode = 500;
            response.statusMessage = "Internal Server Error";
            response.body = "Failed to inspect file";
            response.headers["Content-Type"] = "text/plain; charset=utf-8";
            return true;
        }

        const auto fileSize = std::filesystem::file_size(canonicalPath, error);
        if (error) {
            response.statusCode = 500;
            response.statusMessage = "Internal Server Error";
            response.body = "Failed to inspect file";
            response.headers["Content-Type"] = "text/plain; charset=utf-8";
            return true;
        }

        const std::string contentType = getContentType(canonicalPath.extension().string());
        const std::string etag = buildStaticFileEtag(fileSize, lastWriteTime);
        const std::string lastModified = formatFileTime(lastWriteTime);

        response.headers["Content-Type"] = contentType;
        response.headers["ETag"] = etag;
        response.headers["Last-Modified"] = lastModified;
        response.headers["Accept-Ranges"] = "bytes";

        const std::string rangeHeader = headerValueCaseInsensitive(request.headers, "Range");
        const bool supportsRangeResponse =
            request.method == HttpMethod::GET || request.method == HttpMethod::HEAD;
        const bool hasRangeRequest = !rangeHeader.empty() && supportsRangeResponse;

        if (!hasRangeRequest && isNotModified(request, etag, lastModified)) {
            response.statusCode = 304;
            response.statusMessage = "Not Modified";
            response.body.clear();
            response.headers["Content-Length"] = "0";
            return true;
        }

        if (hasRangeRequest) {
            const auto parsedRange = parseRangeHeader(rangeHeader, fileSize);
            if (!parsedRange.has_value()) {
                response.statusCode = 416;
                response.statusMessage = "Range Not Satisfiable";
                response.body.clear();
                response.headers["Content-Range"] = "bytes */" + std::to_string(fileSize);
                response.headers["Content-Length"] = "0";
                return true;
            }

            const auto [offset, length] = *parsedRange;
            response.statusCode = 206;
            response.statusMessage = "Partial Content";
            response.headers["Content-Range"] =
                "bytes " + std::to_string(offset) + "-" + std::to_string(offset + length - 1) + "/" + std::to_string(fileSize);
            response.headers["Content-Length"] = std::to_string(length);
            if (request.method != HttpMethod::HEAD) {
                if (length > static_cast<uint64_t>(staticFileCacheLimit_)) {
                    response.hasFileBody = true;
                    response.filePath = canonicalPath.string();
                    response.fileOffset = offset;
                    response.fileLength = length;
                    response.body.clear();
                } else {
                    auto fileData = readFileSegment(canonicalPath, offset, length);
                    if (!fileData.has_value()) {
                        response.statusCode = 500;
                        response.statusMessage = "Internal Server Error";
                        response.body = "Failed to read file";
                        response.headers["Content-Type"] = "text/plain; charset=utf-8";
                        response.headers.erase("Content-Range");
                        response.headers["Content-Length"] = std::to_string(response.body.size());
                        return true;
                    }
                    response.body = std::move(*fileData);
                }
            }
            return true;
        }

        response.statusCode = 200;
        response.statusMessage = "OK";
        response.headers["Content-Length"] = std::to_string(fileSize);
        if (request.method == HttpMethod::HEAD) {
            response.body.clear();
            return true;
        }

        if (fileSize > staticFileCacheLimit_) {
            response.hasFileBody = true;
            response.filePath = canonicalPath.string();
            response.fileOffset = 0;
            response.fileLength = static_cast<uint64_t>(fileSize);
            response.body.clear();
            return true;
        }

        auto fileData = loadStaticFile(canonicalPath, fileSize, lastWriteTime, contentType, etag, lastModified);
        if (!fileData.has_value()) {
            response.statusCode = 500;
            response.statusMessage = "Internal Server Error";
            response.body = "Failed to read file";
            response.headers["Content-Type"] = "text/plain; charset=utf-8";
            response.headers["Content-Length"] = std::to_string(response.body.size());
            return true;
        }

        response.body = std::move(*fileData);
        return true;
    }

    void finalizeResponse(const HttpRequest& request, bool keepAlive, HttpResponse& response) const {
        if (!response.headers.count("Date")) {
            response.headers["Date"] = formatHttpDate(std::chrono::system_clock::now());
        }
        if (!response.headers.count("Connection")) {
            response.headers["Connection"] = keepAlive ? "keep-alive" : "close";
        }
        if (!response.headers.count("Content-Type")) {
            response.headers["Content-Type"] = "text/plain; charset=utf-8";
        }
        if (response.hasFileBody && !response.headers.count("Content-Length")) {
            response.headers["Content-Length"] = std::to_string(response.fileLength);
        }

        if (request.method == HttpMethod::HEAD) {
            if (!response.headers.count("Content-Length")) {
                response.headers["Content-Length"] =
                    std::to_string(response.hasFileBody ? response.fileLength : response.body.size());
            }
            response.hasFileBody = false;
            response.filePath.clear();
            response.fileOffset = 0;
            response.fileLength = 0;
            response.body.clear();
            return;
        }

        if (!response.headers.count("Content-Length") &&
            !containsToken(headerValueCaseInsensitiveView(response.headers, "Transfer-Encoding"), "chunked")) {
            response.headers["Content-Length"] =
                std::to_string(response.hasFileBody ? response.fileLength : response.body.size());
        }
    }

    void armRequestTimer(ConnectionId clientId, ClientState& state) {
        if (requestTimeout_ == 0 || !state.requestTimer) {
            return;
        }

        std::weak_ptr<Impl> weakSelf = shared_from_this();
        state.requestTimer->start(std::chrono::milliseconds(requestTimeout_), [weakSelf, clientId]() {
            auto self = weakSelf.lock();
            if (!self) {
                return;
            }
            self->reportError(FASTNET_ERROR(ErrorCode::TimeoutError, "HTTP request timed out"));
            self->tcpServer_.disconnectClient(clientId);
        });
    }

    void stopRequestTimer(ClientState& state) {
        if (state.requestTimer) {
            state.requestTimer->stop();
        }
    }

    static std::string_view makeBufferedView(const ClientState& state) {
        if (state.bufferOffset >= state.buffer.size()) {
            return {};
        }
        return std::string_view(
            reinterpret_cast<const char*>(state.buffer.data() + state.bufferOffset),
            state.buffer.size() - state.bufferOffset);
    }

    static void compactClientBufferLocked(ClientState& state) {
        if (state.bufferOffset == 0) {
            return;
        }
        if (state.bufferOffset >= state.buffer.size()) {
            state.buffer.clear();
            state.bufferOffset = 0;
            return;
        }
        if (state.bufferOffset < 4096 && state.bufferOffset * 2 < state.buffer.size()) {
            return;
        }
        state.buffer.erase(
            state.buffer.begin(),
            state.buffer.begin() + static_cast<std::ptrdiff_t>(state.bufferOffset));
        state.bufferOffset = 0;
    }

    bool isNotModified(const HttpRequest& request,
                       const std::string& etag,
                       const std::string& lastModified) const {
        const std::string_view ifNoneMatch = headerValueCaseInsensitiveView(request.headers, "If-None-Match");
        if (!ifNoneMatch.empty()) {
            size_t offset = 0;
            while (offset <= ifNoneMatch.size()) {
                const size_t separator = ifNoneMatch.find(',', offset);
                const std::string_view token = trimAsciiWhitespace(
                    ifNoneMatch.substr(offset, separator == std::string_view::npos ? std::string_view::npos : separator - offset));
                const bool weakMatch =
                    token.size() == etag.size() + 2 &&
                    token.substr(0, 2) == "W/" &&
                    token.substr(2) == etag;
                if (token == "*" || token == etag || weakMatch) {
                    return true;
                }
                if (separator == std::string_view::npos) {
                    break;
                }
                offset = separator + 1;
            }
        }

        const std::string_view ifModifiedSince = headerValueCaseInsensitiveView(request.headers, "If-Modified-Since");
        return !ifModifiedSince.empty() && ifModifiedSince == lastModified;
    }

    std::optional<std::pair<uint64_t, uint64_t>> parseRangeHeader(std::string_view rangeHeader, uintmax_t fileSize) const {
        if (fileSize == 0) {
            return std::nullopt;
        }

        rangeHeader = trimAsciiWhitespace(rangeHeader);
        if (rangeHeader.size() < 6 || !equalsAsciiIgnoreCase(rangeHeader.substr(0, 6), "bytes=")) {
            return std::nullopt;
        }

        const std::string_view spec = rangeHeader.substr(6);
        if (spec.find(',') != std::string::npos) {
            return std::nullopt;
        }

        const size_t dashPos = spec.find('-');
        if (dashPos == std::string::npos) {
            return std::nullopt;
        }

        try {
            if (dashPos == 0) {
                const uint64_t suffixLength = static_cast<uint64_t>(std::stoull(std::string(spec.substr(1))));
                if (suffixLength == 0) {
                    return std::nullopt;
                }
                const uint64_t length = std::min<uint64_t>(suffixLength, static_cast<uint64_t>(fileSize));
                return std::make_pair(static_cast<uint64_t>(fileSize) - length, length);
            }

            const uint64_t start = static_cast<uint64_t>(std::stoull(std::string(spec.substr(0, dashPos))));
            uint64_t end = static_cast<uint64_t>(fileSize) - 1;
            if (dashPos + 1 < spec.size()) {
                end = static_cast<uint64_t>(std::stoull(std::string(spec.substr(dashPos + 1))));
            }

            if (start >= fileSize || end < start) {
                return std::nullopt;
            }

            end = std::min<uint64_t>(end, static_cast<uint64_t>(fileSize) - 1);
            return std::make_pair(start, end - start + 1);
        } catch (...) {
            return std::nullopt;
        }
    }

    std::optional<std::string> readFileSegment(const std::filesystem::path& path,
                                               uint64_t offset,
                                               uint64_t length) const {
        if (length > static_cast<uint64_t>((std::numeric_limits<size_t>::max)()) ||
            length > static_cast<uint64_t>((std::numeric_limits<std::streamsize>::max)())) {
            return std::nullopt;
        }

        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) {
            return std::nullopt;
        }

        file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        if (!file.good()) {
            return std::nullopt;
        }

        std::string data(static_cast<size_t>(length), '\0');
        file.read(data.data(), static_cast<std::streamsize>(length));
        if (file.bad()) {
            return std::nullopt;
        }
        data.resize(static_cast<size_t>(file.gcount()));
        if (data.size() != length) {
            return std::nullopt;
        }
        return data;
    }

    std::optional<std::string> loadStaticFile(const std::filesystem::path& canonicalPath,
                                              uintmax_t fileSize,
                                              std::filesystem::file_time_type lastWriteTime,
                                              const std::string& contentType,
                                              const std::string& etag,
                                              const std::string& lastModified) const {
        if (fileSize <= staticFileCacheLimit_) {
            const std::string cacheKey = canonicalPath.string();
            {
                std::lock_guard<std::mutex> lock(staticFileCacheMutex_);
                auto it = staticFileCache_.find(cacheKey);
                if (it != staticFileCache_.end() &&
                    it->second.size == fileSize &&
                    it->second.lastWriteTime == lastWriteTime) {
                    return it->second.body;
                }
            }

            auto body = readFileSegment(canonicalPath, 0, static_cast<uint64_t>(fileSize));
            if (!body.has_value()) {
                return std::nullopt;
            }

            StaticFileCacheEntry entry;
            entry.size = fileSize;
            entry.lastWriteTime = lastWriteTime;
            entry.contentType = contentType;
            entry.etag = etag;
            entry.lastModified = lastModified;
            entry.body = *body;

            std::lock_guard<std::mutex> lock(staticFileCacheMutex_);
            staticFileCache_[cacheKey] = std::move(entry);
            return body;
        }

        return readFileSegment(canonicalPath, 0, static_cast<uint64_t>(fileSize));
    }

    static std::string buildStaticFileEtag(uintmax_t fileSize, std::filesystem::file_time_type lastWriteTime) {
        const auto tickCount = lastWriteTime.time_since_epoch().count();
        return "\"" + std::to_string(fileSize) + "-" + std::to_string(tickCount) + "\"";
    }

    static std::chrono::system_clock::time_point toSystemClock(std::filesystem::file_time_type fileTime) {
        return std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            fileTime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
    }

    static std::string formatFileTime(std::filesystem::file_time_type fileTime) {
        return formatHttpDate(toSystemClock(fileTime));
    }

    static std::optional<size_t> getChunkedMessageSize(std::string_view data) {
        size_t offset = 0;
        while (true) {
            const size_t lineEnd = data.find("\r\n", offset);
            if (lineEnd == std::string_view::npos) {
                return std::nullopt;
            }

            const std::string line(data.substr(offset, lineEnd - offset));
            size_t chunkSize = 0;
            try {
                chunkSize = static_cast<size_t>(std::stoull(line, nullptr, 16));
            } catch (...) {
                return 0;
            }

            offset = lineEnd + 2;
            if (chunkSize == 0) {
                if (data.size() < offset + 2) {
                    return std::nullopt;
                }
                if (data.substr(offset, 2) != "\r\n") {
                    return 0;
                }
                return offset + 2;
            }

            if (data.size() < offset + chunkSize + 2) {
                return std::nullopt;
            }
            if (data.substr(offset + chunkSize, 2) != "\r\n") {
                return 0;
            }
            offset += chunkSize + 2;
        }
    }

    bool shouldKeepAlive(const HttpRequestView& requestView) const {
        const auto connection = requestView.getHeader("Connection");
        const bool http11 = requestView.version == "HTTP/1.1";

        if (!connection.has_value()) {
            return http11;
        }

        if (containsToken(*connection, "close")) {
            return false;
        }
        if (containsToken(*connection, "keep-alive")) {
            return true;
        }
        return http11;
    }

    void reportError(const Error& error) const {
        if (serverErrorCallback_) {
            serverErrorCallback_(error);
        }
    }

    static std::optional<HttpMethod> tryStringToMethod(std::string_view method) {
        if (equalsAsciiIgnoreCase(method, "GET")) return HttpMethod::GET;
        if (equalsAsciiIgnoreCase(method, "POST")) return HttpMethod::POST;
        if (equalsAsciiIgnoreCase(method, "PUT")) return HttpMethod::PUT;
        if (equalsAsciiIgnoreCase(method, "DELETE")) return HttpMethod::DELETE_METHOD;
        if (equalsAsciiIgnoreCase(method, "HEAD")) return HttpMethod::HEAD;
        if (equalsAsciiIgnoreCase(method, "OPTIONS")) return HttpMethod::OPTIONS_METHOD;
        if (equalsAsciiIgnoreCase(method, "PATCH")) return HttpMethod::PATCH;
        return std::nullopt;
    }

    static std::string normalizePath(const std::string& path) {
        if (path == "*") {
            return "*";
        }
        if (path.empty()) {
            return "/";
        }

        std::string normalized = path;
        if (normalized.front() != '/') {
            normalized.insert(normalized.begin(), '/');
        }
        while (normalized.size() > 1 && normalized.back() == '/') {
            normalized.pop_back();
        }
        return normalized;
    }

    static std::string normalizeRequestPath(std::string_view path) {
        if (path == "*") {
            return "*";
        }

        const size_t schemePos = path.find("://");
        if (schemePos != std::string_view::npos) {
            const size_t pathPos = path.find('/', schemePos + 3);
            path = pathPos == std::string_view::npos ? std::string_view("/") : path.substr(pathPos);
        }

        if (path.empty()) {
            return "/";
        }

        std::string normalized(path.begin(), path.end());
        if (normalized.front() != '/') {
            normalized.insert(normalized.begin(), '/');
        }
        while (normalized.size() > 1 && normalized.back() == '/') {
            normalized.pop_back();
        }
        return normalized;
    }

    static char toLowerAscii(char c) noexcept {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }

    static std::string_view trimAsciiWhitespace(std::string_view input) noexcept {
        size_t begin = 0;
        size_t end = input.size();
        while (begin < end && std::isspace(static_cast<unsigned char>(input[begin])) != 0) {
            ++begin;
        }
        while (end > begin && std::isspace(static_cast<unsigned char>(input[end - 1])) != 0) {
            --end;
        }
        return input.substr(begin, end - begin);
    }

    static bool equalsAsciiIgnoreCase(std::string_view left, std::string_view right) noexcept {
        if (left.size() != right.size()) {
            return false;
        }
        for (size_t i = 0; i < left.size(); ++i) {
            if (toLowerAscii(left[i]) != toLowerAscii(right[i])) {
                return false;
            }
        }
        return true;
    }

    static bool containsToken(std::string_view value, std::string_view token) {
        size_t offset = 0;
        while (offset <= value.size()) {
            const size_t separator = value.find(',', offset);
            const std::string_view candidate = trimAsciiWhitespace(
                value.substr(offset, separator == std::string_view::npos ? std::string_view::npos : separator - offset));
            if (equalsAsciiIgnoreCase(candidate, token)) {
                return true;
            }
            if (separator == std::string_view::npos) {
                break;
            }
            offset = separator + 1;
        }
        return false;
    }

    static std::string_view headerValueCaseInsensitiveView(const std::map<std::string, std::string>& headers,
                                                           std::string_view name) {
        for (const auto& [key, value] : headers) {
            if (HttpParser::caseInsensitiveCompare(key, name)) {
                return value;
            }
        }
        return {};
    }

    static std::string headerValueCaseInsensitive(const std::map<std::string, std::string>& headers,
                                                  std::string_view name) {
        return std::string(headerValueCaseInsensitiveView(headers, name));
    }

    static std::string formatHttpDate(std::chrono::system_clock::time_point now) {
        const std::time_t timestamp = std::chrono::system_clock::to_time_t(now);
        std::tm utc{};
#ifdef _WIN32
        gmtime_s(&utc, &timestamp);
#else
        gmtime_r(&timestamp, &utc);
#endif

        char buffer[64] = {0};
        std::strftime(buffer, sizeof(buffer), "%a, %d %b %Y %H:%M:%S GMT", &utc);
        return buffer;
    }

    static std::string getContentType(const std::string& extension) {
        static const std::map<std::string, std::string> contentTypes = {
            {".avif", "image/avif"},
            {".css", "text/css"},
            {".csv", "text/csv"},
            {".ico", "image/x-icon"},
            {".gif", "image/gif"},
            {".htm", "text/html; charset=utf-8"},
            {".html", "text/html; charset=utf-8"},
            {".jpeg", "image/jpeg"},
            {".jpg", "image/jpeg"},
            {".js", "application/javascript"},
            {".json", "application/json"},
            {".map", "application/json"},
            {".mjs", "application/javascript"},
            {".pdf", "application/pdf"},
            {".png", "image/png"},
            {".svg", "image/svg+xml"},
            {".txt", "text/plain; charset=utf-8"},
            {".wasm", "application/wasm"},
            {".webp", "image/webp"},
            {".woff", "font/woff"},
            {".woff2", "font/woff2"},
            {".xml", "application/xml"}
        };

        auto it = contentTypes.find(extension);
        return it == contentTypes.end() ? "application/octet-stream" : it->second;
    }

    IoService& ioService_;
    TcpServer tcpServer_;
    bool running_ = false;
    SSLConfig sslConfig_;
    uint32_t connectionTimeout_ = 0;
    uint32_t requestTimeout_ = 30000;
    uint32_t writeTimeout_ = 0;
    size_t maxConnections_ = 10000;
    size_t maxRequestSize_ = 16 * 1024 * 1024;

    mutable std::mutex clientsMutex_;
    std::map<ConnectionId, std::unique_ptr<ClientState>> clients_;

    mutable std::mutex handlersMutex_;
    std::unordered_map<RouteKey, RequestHandler, RouteKeyHash> handlers_;
    std::map<std::string, std::filesystem::path> staticFileHandlers_;
    mutable std::mutex staticFileCacheMutex_;
    mutable std::map<std::string, StaticFileCacheEntry> staticFileCache_;
    size_t staticFileCacheLimit_ = 256 * 1024;
    RequestHandler defaultHandler_;
    HttpServerErrorCallback serverErrorCallback_;
};

HttpServer::HttpServer(IoService& ioService)
    : impl_(std::make_shared<Impl>(ioService)) {
    impl_->initializeCallbacks();
}

HttpServer::~HttpServer() = default;

Error HttpServer::start(uint16_t port, const std::string& bindAddress, const SSLConfig& sslConfig) {
    return impl_->start(port, bindAddress, sslConfig);
}

Error HttpServer::start(const Address& listenAddress, const SSLConfig& sslConfig) {
    return impl_->start(listenAddress.port, listenAddress.host(), sslConfig);
}

void HttpServer::stop() {
    impl_->stop();
}

void HttpServer::registerHandler(const std::string& path, HttpMethod method, const RequestHandler& handler) {
    impl_->registerHandler(path, method, handler);
}

void HttpServer::registerGet(std::string_view path, const RequestHandler& handler) {
    impl_->registerHandler(path, HttpMethod::GET, handler);
}

void HttpServer::registerPost(std::string_view path, const RequestHandler& handler) {
    impl_->registerHandler(path, HttpMethod::POST, handler);
}

void HttpServer::registerPut(std::string_view path, const RequestHandler& handler) {
    impl_->registerHandler(path, HttpMethod::PUT, handler);
}

void HttpServer::registerDelete(std::string_view path, const RequestHandler& handler) {
    impl_->registerHandler(path, HttpMethod::DELETE_METHOD, handler);
}

void HttpServer::registerPatch(std::string_view path, const RequestHandler& handler) {
    impl_->registerHandler(path, HttpMethod::PATCH, handler);
}

void HttpServer::registerHead(std::string_view path, const RequestHandler& handler) {
    impl_->registerHandler(path, HttpMethod::HEAD, handler);
}

void HttpServer::registerOptions(std::string_view path, const RequestHandler& handler) {
    impl_->registerHandler(path, HttpMethod::OPTIONS_METHOD, handler);
}

void HttpServer::registerStaticFileHandler(const std::string& pathPrefix, const std::string& directory) {
    impl_->registerStaticFileHandler(pathPrefix, directory);
}

void HttpServer::setRequestHandler(const RequestHandler& handler) {
    impl_->setRequestHandler(handler);
}

void HttpServer::setConnectionTimeout(uint32_t timeoutMs) {
    impl_->setConnectionTimeout(timeoutMs);
}

void HttpServer::setRequestTimeout(uint32_t timeoutMs) {
    impl_->setRequestTimeout(timeoutMs);
}

void HttpServer::setWriteTimeout(uint32_t timeoutMs) {
    impl_->setWriteTimeout(timeoutMs);
}

void HttpServer::setMaxConnections(size_t maxConnections) {
    impl_->setMaxConnections(maxConnections);
}

void HttpServer::setMaxRequestSize(size_t bytes) {
    impl_->setMaxRequestSize(bytes);
}

void HttpServer::setStaticFileCacheLimit(size_t bytes) {
    impl_->setStaticFileCacheLimit(bytes);
}

void HttpServer::setSSLConfig(const SSLConfig& sslConfig) {
    impl_->setSSLConfig(sslConfig);
}

void HttpServer::setServerErrorCallback(const HttpServerErrorCallback& callback) {
    impl_->setServerErrorCallback(callback);
}

size_t HttpServer::getClientCount() const {
    return impl_->getClientCount();
}

std::vector<ConnectionId> HttpServer::getClientIds() const {
    return impl_->getClientIds();
}

Address HttpServer::getListenAddress() const {
    return impl_->getListenAddress();
}

bool HttpServer::isRunning() const {
    return impl_->isRunning();
}

} // namespace FastNet
