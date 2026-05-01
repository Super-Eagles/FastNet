/**
 * @file HttpServer.h
 * @brief FastNet HTTP server API
 */
#pragma once

#include "Config.h"
#include "Error.h"
#include "HttpCommon.h"
#include "IoService.h"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace FastNet {

struct HttpRequest {
    HttpMethod method = HttpMethod::GET;
    std::string methodName = "GET";
    std::string target;
    std::string path;
    std::string queryString;
    std::string version = "HTTP/1.1";
    std::map<std::string, std::string> queryParams;
    std::map<std::string, std::string> headers;
    std::string body;
    Address clientAddress;

    std::optional<std::string> getHeader(std::string_view name) const;
};

using RequestHandler = std::function<void(const HttpRequest& request, HttpResponse& response)>;
using HttpServerErrorCallback = std::function<void(const Error& error)>;

class FASTNET_API HttpServer {
public:
    explicit HttpServer(IoService& ioService);
    ~HttpServer();

    Error start(uint16_t port,
                const std::string& bindAddress = "0.0.0.0",
                const SSLConfig& sslConfig = SSLConfig());
    Error start(const Address& listenAddress, const SSLConfig& sslConfig = SSLConfig());
    void stop();

    void registerHandler(const std::string& path, HttpMethod method, const RequestHandler& handler);
    void registerGet(std::string_view path, const RequestHandler& handler);
    void registerPost(std::string_view path, const RequestHandler& handler);
    void registerPut(std::string_view path, const RequestHandler& handler);
    void registerDelete(std::string_view path, const RequestHandler& handler);
    void registerPatch(std::string_view path, const RequestHandler& handler);
    void registerHead(std::string_view path, const RequestHandler& handler);
    void registerOptions(std::string_view path, const RequestHandler& handler);
    void registerStaticFileHandler(const std::string& pathPrefix, const std::string& directory);
    void setRequestHandler(const RequestHandler& handler);

    void setConnectionTimeout(uint32_t timeoutMs);
    void setRequestTimeout(uint32_t timeoutMs);
    void setWriteTimeout(uint32_t timeoutMs);
    void setMaxConnections(size_t maxConnections);
    void setMaxRequestSize(size_t bytes);
    void setStaticFileCacheLimit(size_t bytes);
    void setSSLConfig(const SSLConfig& sslConfig);
    void setServerErrorCallback(const HttpServerErrorCallback& callback);

    size_t getClientCount() const;
    std::vector<ConnectionId> getClientIds() const;
    Address getListenAddress() const;
    bool isRunning() const;

private:
    class Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace FastNet
