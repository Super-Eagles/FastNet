/**
 * @file WebSocketProtocol.h
 * @brief WebSocket frame and handshake helpers
 */
#pragma once

#include "Config.h"

#include <string>
#include <string_view>

namespace FastNet {

enum class WSFrameType {
    Text,
    Binary,
    Close,
    Ping,
    Pong
};

struct WSFrameMetadata {
    WSFrameType type = WSFrameType::Text;
    bool final = true;
    bool masked = false;
};

class FASTNET_API WebSocketProtocol {
public:
    static std::string createHandshakeKey();
    static std::string createAcceptKey(std::string_view key);
    static std::string createHandshakeResponse(std::string_view key);

    static std::string encodeFrame(std::string_view data,
                                   WSFrameType type = WSFrameType::Text,
                                   bool maskPayload = false);
    static std::string encodeFrame(const Buffer& data,
                                   WSFrameType type = WSFrameType::Binary,
                                   bool maskPayload = false);

    // This overload accepts only complete FIN frames and treats fragmentation as a protocol error.
    static bool decodeFrame(const Buffer& data, std::string& payload, WSFrameType& type);

    // This overload exposes frame metadata for callers that need to reason about masking or FIN bits.
    static bool decodeFrame(std::string_view data, std::string& payload, WSFrameMetadata& metadata);
    static bool decodeFrame(std::string_view data, Buffer& payload, WSFrameMetadata& metadata);

    static bool isControlFrame(WSFrameType type) noexcept;
    static bool isValidClosePayload(std::string_view payload) noexcept;
};

} // namespace FastNet
