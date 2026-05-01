/**
 * @file HttpClient.h
 * @brief FastNet HTTP client API
 */
#pragma once

#include "Config.h"
#include "Error.h"
#include "HttpCommon.h"
#include "IoService.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>

namespace FastNet {

using RequestHeaders = std::map<std::string, std::string>;
using HttpClientConnectCallback = std::function<void(bool success, const std::string& errorMessage)>;
using HttpClientResponseCallback = std::function<void(const HttpResponse& response)>;

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

    void disconnect();

    void setConnectTimeout(uint32_t timeoutMs);
    void setRequestTimeout(uint32_t timeoutMs);
    void setReadTimeout(uint32_t timeoutMs);
    void setFollowRedirects(bool follow);
    void setMaxRedirects(uint32_t maxRedirects);
    void setUseCompression(bool use);
    void setSSLConfig(const SSLConfig& sslConfig);

    bool isConnected() const;
    Address getLocalAddress() const;
    Address getRemoteAddress() const;
    Error getLastError() const;

private:
    class Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace FastNet
