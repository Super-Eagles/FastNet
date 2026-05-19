/**
 * @file HttpClient.h
 * @brief FastNet HTTP client API
 */
#pragma once

#include "Config.h"
#include "Error.h"
#include "base64.h"
#include "HttpCommon.h"
#include "HttpParser.h"
#include "IoService.h"

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>

namespace FastNet {

using RequestHeaders = HttpHeaders;
using HttpClientConnectCallback = std::function<void(bool success, const std::string& errorMessage)>;
using HttpClientResponseCallback = std::function<void(const HttpResponse& response)>;
using HttpClientStreamHeadersCallback = std::function<void(const HttpResponse& response)>;
using HttpClientStreamDataCallback = std::function<bool(std::string_view chunk)>;
using HttpClientStreamCompleteCallback = std::function<void(const HttpResponse& response)>;

struct FASTNET_API HttpProxyOptions {
    std::string host;
    uint16_t port = 0;
    RequestHeaders headers;
    std::string username;
    std::string password;

    bool enabled() const noexcept {
        return !host.empty() && port != 0;
    }
};

class FASTNET_API HttpClientRequest {
public:
    explicit HttpClientRequest(std::string path = "/");

    HttpClientRequest& setMethod(std::string method);
    HttpClientRequest& setPath(std::string path);
    HttpClientRequest& addHeader(std::string name, std::string value);
    HttpClientRequest& addQuery(std::string name, std::string value);
    HttpClientRequest& setBody(std::string body);
    HttpClientRequest& setJson(std::string json);
    HttpClientRequest& setForm(const std::map<std::string, std::string>& values);
    HttpClientRequest& setBearerToken(std::string token);
    HttpClientRequest& setBasicAuth(std::string username, std::string password);
    HttpClientRequest& setUserAgent(std::string userAgent);
    HttpClientRequest& setAccept(std::string accept);
    HttpClientRequest& setTimeout(std::chrono::milliseconds timeout);

    const std::string& method() const noexcept;
    const std::string& path() const noexcept;
    const RequestHeaders& headers() const noexcept;
    const std::string& body() const noexcept;
    uint32_t timeoutMs() const noexcept;

private:
    std::string method_ = "GET";
    std::string path_;
    RequestHeaders headers_;
    std::string body_;
    uint32_t timeoutMs_ = 0;
};

class FASTNET_API HttpClient {
public:
    explicit HttpClient(IoService& ioService);
    ~HttpClient();

    bool connect(const std::string& url, const HttpClientConnectCallback& callback);
    bool get(const std::string& path, const RequestHeaders& headers, const HttpClientResponseCallback& callback);
    bool head(const std::string& path, const RequestHeaders& headers, const HttpClientResponseCallback& callback);
    bool post(const std::string& path,
              const RequestHeaders& headers,
              const std::string& body,
              const HttpClientResponseCallback& callback);
    bool put(const std::string& path,
             const RequestHeaders& headers,
             const std::string& body,
             const HttpClientResponseCallback& callback);
    bool patch(const std::string& path,
               const RequestHeaders& headers,
               const std::string& body,
               const HttpClientResponseCallback& callback);
    bool del(const std::string& path, const RequestHeaders& headers, const HttpClientResponseCallback& callback);
    bool request(std::string_view method,
                 std::string_view path,
                 const RequestHeaders& headers,
                 std::string_view body,
                 const HttpClientResponseCallback& callback);
    bool request(const HttpClientRequest& request, const HttpClientResponseCallback& callback);
    bool streamRequest(std::string_view method,
                       std::string_view path,
                       const RequestHeaders& headers,
                       std::string_view body,
                       const HttpClientStreamHeadersCallback& headersCallback,
                       const HttpClientStreamDataCallback& dataCallback,
                       const HttpClientStreamCompleteCallback& completeCallback);
    bool streamRequest(const HttpClientRequest& request,
                       const HttpClientStreamHeadersCallback& headersCallback,
                       const HttpClientStreamDataCallback& dataCallback,
                       const HttpClientStreamCompleteCallback& completeCallback);
    bool streamGet(const std::string& path,
                   const RequestHeaders& headers,
                   const HttpClientStreamHeadersCallback& headersCallback,
                   const HttpClientStreamDataCallback& dataCallback,
                   const HttpClientStreamCompleteCallback& completeCallback);

    void cancelRequest();
    void disconnect();

    void setConnectTimeout(uint32_t timeoutMs);
    void setRequestTimeout(uint32_t timeoutMs);
    void setReadTimeout(uint32_t timeoutMs);
    void setFollowRedirects(bool follow);
    void setMaxRedirects(uint32_t maxRedirects);
    void setUseCompression(bool use);
    void setSSLConfig(const SSLConfig& sslConfig);
    bool setProxyUrl(std::string_view proxyUrl);
    void setProxy(const HttpProxyOptions& proxyOptions);
    void clearProxy();

    bool isConnected() const;
    Address getLocalAddress() const;
    Address getRemoteAddress() const;
    Error getLastError() const;

private:
    class Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace FastNet
