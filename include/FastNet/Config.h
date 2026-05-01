/**
 * @file Config.h
 * @brief FastNet网络库配置头文件
 * @author slob
 * @version 1.3.0
 * @date 2025-12-13
 *
 * 定义FastNet网络库的基础配置、类型定义和跨平台兼容性处理
 */
#pragma once
// FastNet版本号定义
#define FASTNET_VERSION_MAJOR 1
#define FASTNET_VERSION_MINOR 4
#define FASTNET_VERSION_PATCH 0
#define FASTNET_VERSION "1.4.0"
// 编译器优化提示宏 — 分支预测 hint
// C++20 [[likely]]/[[unlikely]] 属性所有主流编译器均支持；
// 对于 C++17 模式下的 GCC/Clang 退回到 __builtin_expect。
#if defined(__cplusplus) && __cplusplus >= 202002L
    // C++20: use standard attributes directly in calling code; keep macros as
    // wrappers that evaluate to the condition unchanged (attributes are at call site).
    #define likely(x)   (x)
    #define unlikely(x) (x)
#elif defined(_MSC_VER)
    #define likely(x)   (x)
    #define unlikely(x) (x)
#else
    #define likely(x)   __builtin_expect(!!(x), 1)
    #define unlikely(x) __builtin_expect(!!(x), 0)
#endif
// 跨平台兼容性定义
#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #define FASTNET_WINDOWS
    #ifdef FASTNET_STATIC_LIB
        // 静态库不使用导出声明
        #define FASTNET_API
    #elif defined(FASTNET_EXPORTS)
        #define FASTNET_API __declspec(dllexport)
    #else
        #define FASTNET_API __declspec(dllimport)
    #endif
#else
    #define FASTNET_API
#endif
// 平台检测
#if defined(__linux__)
    #define FASTNET_LINUX
#elif defined(__APPLE__)
    #define FASTNET_MACOS
#endif
// 架构检测
#if defined(_WIN64) || defined(__x86_64__) || defined(__ppc64__)
    #define FASTNET_64BIT
#else
    #define FASTNET_32BIT
#endif
// 编译配置检测
#ifdef NDEBUG
    #define FASTNET_RELEASE
#else
    #define FASTNET_DEBUG
#endif
#include <string>
#include <string_view>
#include <memory>
#include <functional>
#include <cstdint>
#include <cctype>
#include <atomic>
#include <optional>
#include <utility>
#include <vector>
#include <unordered_map>
namespace FastNet {
    /**
     * @struct Address
     * @brief 网络地址结构
     */
    struct FASTNET_API Address {
        std::string ip;
        uint16_t port = 0;

        Address() = default;
        Address(std::string ip_, uint16_t port_)
            : ip(std::move(ip_)),
              port(port_) {}

        const std::string& host() const noexcept {
            return ip;
        }

        std::string& host() noexcept {
            return ip;
        }

        bool hasPort() const noexcept {
            return port != 0;
        }

        std::string normalizedHost() const {
            return std::string(stripBrackets(ip));
        }

        static std::string_view stripBrackets(std::string_view hostName) {
            if (hostName.size() >= 2 && hostName.front() == '[' && hostName.back() == ']') {
                return hostName.substr(1, hostName.size() - 2);
            }
            return hostName;
        }

        static bool isValidIPv4(std::string_view ipAddress) {
            int parts = 0;
            size_t start = 0;
            while (start <= ipAddress.size()) {
                const size_t end = ipAddress.find('.', start);
                const std::string_view part =
                    end == std::string_view::npos ? ipAddress.substr(start) : ipAddress.substr(start, end - start);
                if (part.empty() || part.size() > 3) {
                    return false;
                }

                int value = 0;
                for (char ch : part) {
                    if (!std::isdigit(static_cast<unsigned char>(ch))) {
                        return false;
                    }
                    value = value * 10 + (ch - '0');
                }
                if (value > 255) {
                    return false;
                }

                ++parts;
                if (end == std::string_view::npos) {
                    break;
                }
                start = end + 1;
            }

            return parts == 4;
        }

        static bool isValidIPv6(std::string_view hostName) {
            hostName = stripBrackets(hostName);
            if (hostName.empty()) {
                return false;
            }

            size_t groups = 0;
            bool compressed = false;
            bool sawIpv4Tail = false;
            size_t index = 0;

            while (index < hostName.size()) {
                if (hostName[index] == ':') {
                    if (index + 1 < hostName.size() && hostName[index + 1] == ':' && !compressed) {
                        compressed = true;
                        index += 2;
                        if (index >= hostName.size()) {
                            break;
                        }
                        continue;
                    }
                    return false;
                }

                const size_t start = index;
                while (index < hostName.size() && hostName[index] != ':') {
                    ++index;
                }

                const std::string_view token = hostName.substr(start, index - start);
                if (token.find('.') != std::string_view::npos) {
                    if (!isValidIPv4(token)) {
                        return false;
                    }
                    groups += 2;
                    sawIpv4Tail = true;
                    if (index != hostName.size()) {
                        return false;
                    }
                    break;
                }

                if (token.empty() || token.size() > 4) {
                    return false;
                }
                for (char ch : token) {
                    if (!std::isxdigit(static_cast<unsigned char>(ch))) {
                        return false;
                    }
                }

                ++groups;
                if (groups > 8) {
                    return false;
                }

                if (index < hostName.size()) {
                    if (hostName[index] != ':') {
                        return false;
                    }
                    if (index + 1 < hostName.size() && hostName[index + 1] == ':') {
                        if (compressed) {
                            return false;
                        }
                        compressed = true;
                        index += 2;
                        if (index >= hostName.size()) {
                            break;
                        }
                    } else {
                        ++index;
                    }
                }
            }

            if (sawIpv4Tail && groups > 8) {
                return false;
            }
            return compressed ? groups < 8 : groups == 8;
        }

        static bool isValidHostname(std::string_view hostName) {
            if (hostName.empty() || hostName.size() > 253) {
                return false;
            }

            size_t labelLength = 0;
            for (size_t index = 0; index < hostName.size(); ++index) {
                const char ch = hostName[index];
                if (ch == '.') {
                    if (labelLength == 0 || hostName[index - 1] == '-') {
                        return false;
                    }
                    labelLength = 0;
                    continue;
                }

                if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '-')) {
                    return false;
                }
                if (ch == '-' && labelLength == 0) {
                    return false;
                }

                ++labelLength;
                if (labelLength > 63) {
                    return false;
                }
            }

            return labelLength != 0 && hostName.front() != '.' && hostName.back() != '-';
        }

        static bool isValidHost(std::string_view hostName) {
            hostName = stripBrackets(hostName);
            return !hostName.empty() &&
                    (hostName == "localhost" || isValidIPv4(hostName) || isValidIPv6(hostName) ||
                     isValidHostname(hostName));
        }

        static bool isLoopbackHost(std::string_view hostName) {
            hostName = stripBrackets(hostName);
            if (hostName == "localhost" || hostName == "::1") {
                return true;
            }
            if (!isValidIPv4(hostName)) {
                return false;
            }
            return hostName.size() >= 4 && hostName.substr(0, 4) == "127.";
        }

        static bool isAnyHost(std::string_view hostName) {
            hostName = stripBrackets(hostName);
            return hostName == "0.0.0.0" || hostName == "::";
        }

        static bool isValidPort(uint16_t portNumber) {
            return portNumber > 0;
        }

        static std::optional<Address> parse(std::string_view endpoint, uint16_t defaultPort = 0) {
            auto trim = [](std::string_view value) {
                while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
                    value.remove_prefix(1);
                }
                while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
                    value.remove_suffix(1);
                }
                return value;
            };
            auto parsePortValue = [](std::string_view portText) -> std::optional<uint16_t> {
                if (portText.empty()) {
                    return std::nullopt;
                }
                unsigned long value = 0;
                for (char ch : portText) {
                    if (std::isdigit(static_cast<unsigned char>(ch)) == 0) {
                        return std::nullopt;
                    }
                    value = (value * 10) + static_cast<unsigned long>(ch - '0');
                    if (value > 65535UL) {
                        return std::nullopt;
                    }
                }
                return static_cast<uint16_t>(value);
            };

            endpoint = trim(endpoint);
            if (endpoint.empty()) {
                return std::nullopt;
            }

            std::string_view hostPart = endpoint;
            uint16_t portValue = defaultPort;

            if (endpoint.front() == '[') {
                const size_t closingBracket = endpoint.find(']');
                if (closingBracket == std::string_view::npos) {
                    return std::nullopt;
                }
                hostPart = endpoint.substr(1, closingBracket - 1);
                if (closingBracket + 1 < endpoint.size()) {
                    if (endpoint[closingBracket + 1] != ':') {
                        return std::nullopt;
                    }
                    const auto parsedPort = parsePortValue(endpoint.substr(closingBracket + 2));
                    if (!parsedPort.has_value()) {
                        return std::nullopt;
                    }
                    portValue = *parsedPort;
                }
            } else {
                const size_t firstColon = endpoint.find(':');
                const size_t lastColon = endpoint.rfind(':');
                if (firstColon != std::string_view::npos && firstColon == lastColon) {
                    hostPart = endpoint.substr(0, firstColon);
                    const auto parsedPort = parsePortValue(endpoint.substr(firstColon + 1));
                    if (!parsedPort.has_value()) {
                        return std::nullopt;
                    }
                    portValue = *parsedPort;
                } else if (firstColon == std::string_view::npos) {
                    hostPart = endpoint;
                } else {
                    hostPart = endpoint;
                }
            }

            hostPart = trim(hostPart);
            if (hostPart.empty()) {
                return std::nullopt;
            }

            Address address(std::string(hostPart), portValue);
            if (!isValidHost(address.ip) || (address.hasPort() && !isValidPort(address.port))) {
                return std::nullopt;
            }
            return address;
        }

        bool isValid() const {
            return isValidHost(ip) && isValidPort(port);
        }

        bool isIPv6() const {
            return isValidIPv6(ip);
        }

        bool isLoopback() const {
            return isLoopbackHost(ip);
        }

        bool isAnyAddress() const {
            return isAnyHost(ip);
        }

        std::string toString() const {
            // Avoid double-copy: build directly with string_view for IPv6.
            if (isIPv6()) {
                const std::string_view bare = stripBrackets(ip);
                std::string result;
                result.reserve(bare.size() + 10);
                result.push_back('[');
                result.append(bare.data(), bare.size());
                result += "]:"; 
                result.append(std::to_string(port));
                return result;
            }
            const std::string_view bare = stripBrackets(ip);
            std::string result;
            result.reserve(bare.size() + 8);
            result.append(bare.data(), bare.size());
            result.push_back(':');
            result.append(std::to_string(port));
            return result;
        }

        bool operator==(const Address& other) const {
            return stripBrackets(ip) == stripBrackets(other.ip) && port == other.port;
        }

        bool operator!=(const Address& other) const {
            return !(*this == other);
        }
    };
    // 网络错误码
    enum class ErrorCode {
        Success = 0,           // 成功
        SocketError,           // 套接字错误
        ConnectionError,       // 连接错误
        BindError,             // 绑定错误
        ListenError,           // 监听错误
        ResolveError,          // 解析错误
        TimeoutError,          // 超时错误
        InvalidArgument,       // 无效参数
        UnknownError,          // 未知错误
        AlreadyRunning,        // 服务器已在运行
        // HTTP相关错误
        HttpProtocolError,     // HTTP协议错误
        HttpResponseError,     // HTTP响应错误
        HttpRedirectError,     // HTTP重定向错误
        // WebSocket相关错误
        WebSocketHandshakeError,   // WebSocket握手错误
        WebSocketProtocolError,    // WebSocket协议错误
        WebSocketFrameError,       // WebSocket帧格式错误
        WebSocketPayloadTooLarge,  // WebSocket负载过大
        WebSocketCloseError,       // WebSocket关闭错误
        WebSocketPingPongTimeout,  // WebSocket Ping/Pong超时错误
        WebSocketConnectionError,  // WebSocket连接错误
        // SSL/TLS相关错误
        SSLError,              // SSL错误
        SSLHandshakeError,     // SSL握手错误
        SSLCertificateError,   // SSL证书错误
        // 认证相关错误
        AuthenticationError,   // 认证错误
        AuthorizationError,    // 授权错误
        // 压缩相关错误
        CompressionError,      // 压缩错误
        DecompressionError     // 解压缩错误
    };
    // 连接ID类型
    using ConnectionId = uint64_t;
    // 全局连接ID生成函数
    FASTNET_API ConnectionId generateConnectionId();
    // 缓冲区类型（向后兼容 std::vector<uint8_t>）
    // 推荐高性能场景使用 FastBuffer（见 FastBuffer.h）
    using Buffer = std::vector<uint8_t>;
    // SSL配置结构
    struct FASTNET_API SSLConfig {
        bool enableSSL = false;              // 是否启用SSL
        std::string certificateFile;         // 证书文件路径
        std::string privateKeyFile;          // 私钥文件路径
        std::string caFile;                  // CA证书文件路径
        bool verifyPeer = true;              // 是否验证对端证书
        std::string hostnameVerification;    // 主机名验证
        SSLConfig() = default;
        SSLConfig(bool enable) : enableSSL(enable) {}
    };
} // namespace FastNet
