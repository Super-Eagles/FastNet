/**
 * @file WebSocketProtocol.cpp
 * @brief FastNet WebSocket 协议实现
 */
#include "WebSocketProtocol.h"

#include "base64.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <random>
#include <string_view>
#include <thread>

namespace FastNet {

namespace {

class Sha1 {
public:
    Sha1() {
        reset();
    }

    void update(const std::uint8_t* data, size_t length) {
        totalBits_ += static_cast<std::uint64_t>(length) * 8;

        while (length > 0) {
            const size_t copyLength = std::min(length, block_.size() - blockLength_);
            std::memcpy(block_.data() + blockLength_, data, copyLength);
            blockLength_ += copyLength;
            data += copyLength;
            length -= copyLength;

            if (blockLength_ == block_.size()) {
                processBlock(block_.data());
                blockLength_ = 0;
            }
        }
    }

    std::array<std::uint8_t, 20> finalize() {
        block_[blockLength_++] = 0x80;

        if (blockLength_ > 56) {
            while (blockLength_ < block_.size()) {
                block_[blockLength_++] = 0;
            }
            processBlock(block_.data());
            blockLength_ = 0;
        }

        while (blockLength_ < 56) {
            block_[blockLength_++] = 0;
        }

        for (int i = 7; i >= 0; --i) {
            block_[blockLength_++] = static_cast<std::uint8_t>((totalBits_ >> (i * 8)) & 0xFF);
        }
        processBlock(block_.data());

        std::array<std::uint8_t, 20> digest{};
        for (size_t i = 0; i < state_.size(); ++i) {
            digest[i * 4 + 0] = static_cast<std::uint8_t>((state_[i] >> 24) & 0xFF);
            digest[i * 4 + 1] = static_cast<std::uint8_t>((state_[i] >> 16) & 0xFF);
            digest[i * 4 + 2] = static_cast<std::uint8_t>((state_[i] >> 8) & 0xFF);
            digest[i * 4 + 3] = static_cast<std::uint8_t>(state_[i] & 0xFF);
        }
        return digest;
    }

private:
    void reset() {
        state_[0] = 0x67452301;
        state_[1] = 0xEFCDAB89;
        state_[2] = 0x98BADCFE;
        state_[3] = 0x10325476;
        state_[4] = 0xC3D2E1F0;
        blockLength_ = 0;
        totalBits_ = 0;
        block_.fill(0);
    }

    static std::uint32_t leftRotate(std::uint32_t value, std::uint32_t bits) {
        return (value << bits) | (value >> (32 - bits));
    }

    void processBlock(const std::uint8_t* block) {
        std::array<std::uint32_t, 80> schedule{};
        for (size_t i = 0; i < 16; ++i) {
            schedule[i] = (static_cast<std::uint32_t>(block[i * 4 + 0]) << 24) |
                          (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
                          (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) |
                          static_cast<std::uint32_t>(block[i * 4 + 3]);
        }
        for (size_t i = 16; i < schedule.size(); ++i) {
            schedule[i] = leftRotate(schedule[i - 3] ^ schedule[i - 8] ^ schedule[i - 14] ^ schedule[i - 16], 1);
        }

        std::uint32_t a = state_[0];
        std::uint32_t b = state_[1];
        std::uint32_t c = state_[2];
        std::uint32_t d = state_[3];
        std::uint32_t e = state_[4];

        for (size_t i = 0; i < schedule.size(); ++i) {
            std::uint32_t f = 0;
            std::uint32_t k = 0;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }

            const std::uint32_t temp = leftRotate(a, 5) + f + e + k + schedule[i];
            e = d;
            d = c;
            c = leftRotate(b, 30);
            b = a;
            a = temp;
        }

        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
    }

    std::array<std::uint32_t, 5> state_{};
    std::array<std::uint8_t, 64> block_{};
    size_t blockLength_ = 0;
    std::uint64_t totalBits_ = 0;
};

std::string sha1Digest(std::string_view input) {
    Sha1 sha1;
    sha1.update(reinterpret_cast<const std::uint8_t*>(input.data()), input.size());
    const auto digest = sha1.finalize();
    return std::string(reinterpret_cast<const char*>(digest.data()), digest.size());
}

std::uint8_t opcodeForType(WSFrameType type) {
    switch (type) {
        case WSFrameType::Text:
            return 0x01;
        case WSFrameType::Binary:
            return 0x02;
        case WSFrameType::Close:
            return 0x08;
        case WSFrameType::Ping:
            return 0x09;
        case WSFrameType::Pong:
            return 0x0A;
        default:
            return 0x01;
    }
}

bool frameTypeFromOpcode(std::uint8_t opcode, WSFrameType& type) {
    switch (opcode) {
        case 0x01:
            type = WSFrameType::Text;
            return true;
        case 0x02:
            type = WSFrameType::Binary;
            return true;
        case 0x08:
            type = WSFrameType::Close;
            return true;
        case 0x09:
            type = WSFrameType::Ping;
            return true;
        case 0x0A:
            type = WSFrameType::Pong;
            return true;
        default:
            return false;
    }
}

bool isControlOpcode(std::uint8_t opcode) {
    return opcode == 0x08 || opcode == 0x09 || opcode == 0x0A;
}

std::mt19937_64& randomEngine() {
    thread_local std::mt19937_64 engine([]() {
        std::random_device rd;
        const auto now = static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        const auto threadSeed =
            static_cast<std::uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
        std::seed_seq seed{
            rd(),
            rd(),
            rd(),
            rd(),
            static_cast<std::uint32_t>(now & 0xFFFFFFFFu),
            static_cast<std::uint32_t>((now >> 32) & 0xFFFFFFFFu),
            static_cast<std::uint32_t>(threadSeed & 0xFFFFFFFFu),
            static_cast<std::uint32_t>((threadSeed >> 32) & 0xFFFFFFFFu),
        };
        return std::mt19937_64(seed);
    }());
    return engine;
}

void fillRandomBytes(std::uint8_t* destination, size_t length) {
    auto& engine = randomEngine();
    while (length >= sizeof(std::uint64_t)) {
        const std::uint64_t value = engine();
        std::memcpy(destination, &value, sizeof(value));
        destination += sizeof(value);
        length -= sizeof(value);
    }

    if (length > 0) {
        const std::uint64_t value = engine();
        std::memcpy(destination, &value, length);
    }
}

size_t frameHeaderSize(std::uint64_t payloadLength, bool masked) noexcept {
    size_t headerSize = 2;
    if (payloadLength >= 126 && payloadLength <= 0xFFFF) {
        headerSize += 2;
    } else if (payloadLength > 0xFFFF) {
        headerSize += 8;
    }
    if (masked) {
        headerSize += 4;
    }
    return headerSize;
}

size_t writePayloadLength(char* destination, std::uint64_t payloadLength, bool masked) noexcept {
    size_t offset = 0;
    const std::uint8_t maskBit = masked ? 0x80 : 0x00;
    if (payloadLength < 126) {
        destination[offset++] = static_cast<char>(maskBit | payloadLength);
        return offset;
    }
    if (payloadLength <= 0xFFFF) {
        destination[offset++] = static_cast<char>(maskBit | 126);
        destination[offset++] = static_cast<char>((payloadLength >> 8) & 0xFF);
        destination[offset++] = static_cast<char>(payloadLength & 0xFF);
        return offset;
    }

    destination[offset++] = static_cast<char>(maskBit | 127);
    for (int shift = 56; shift >= 0; shift -= 8) {
        destination[offset++] = static_cast<char>((payloadLength >> shift) & 0xFF);
    }
    return offset;
}

std::array<std::uint8_t, 4> generateMaskKey() {
    std::array<std::uint8_t, 4> mask{};
    fillRandomBytes(mask.data(), mask.size());
    return mask;
}

} // namespace

std::string WebSocketProtocol::createHandshakeKey() {
    std::array<std::uint8_t, 16> entropy{};
    fillRandomBytes(entropy.data(), entropy.size());
    return base64Encode(std::string_view(
        reinterpret_cast<const char*>(entropy.data()),
        entropy.size()));
}

std::string WebSocketProtocol::createAcceptKey(std::string_view key) {
    static constexpr std::string_view kGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::string source;
    source.reserve(key.size() + kGuid.size());
    source.append(key.data(), key.size());
    source.append(kGuid.data(), kGuid.size());
    return base64Encode(sha1Digest(source));
}

std::string WebSocketProtocol::createHandshakeResponse(std::string_view key) {
    std::string response;
    response.reserve(160);
    response += "HTTP/1.1 101 Switching Protocols\r\n";
    response += "Upgrade: websocket\r\n";
    response += "Connection: Upgrade\r\n";
    response += "Sec-WebSocket-Accept: ";
    response += createAcceptKey(key);
    response += "\r\n\r\n";
    return response;
}

std::string WebSocketProtocol::encodeFrame(std::string_view data, WSFrameType type, bool maskPayload) {
    const std::uint64_t payloadLength = static_cast<std::uint64_t>(data.size());
    const size_t headerSize = frameHeaderSize(payloadLength, maskPayload);
    std::string frame(headerSize + data.size(), '\0');

    size_t offset = 0;
    frame[offset++] = static_cast<char>(0x80 | opcodeForType(type));
    offset += writePayloadLength(frame.data() + offset, payloadLength, maskPayload);

    if (!maskPayload) {
        std::memcpy(frame.data() + offset, data.data(), data.size());
        return frame;
    }

    const auto mask = generateMaskKey();
    std::memcpy(frame.data() + offset, mask.data(), mask.size());
    offset += mask.size();
    for (size_t i = 0; i < data.size(); ++i) {
        frame[offset + i] =
            static_cast<char>(static_cast<std::uint8_t>(data[i]) ^ mask[i & 0x03]);
    }
    return frame;
}

std::string WebSocketProtocol::encodeFrame(const Buffer& data, WSFrameType type, bool maskPayload) {
    return encodeFrame(std::string_view(reinterpret_cast<const char*>(data.data()), data.size()), type, maskPayload);
}

bool WebSocketProtocol::decodeFrame(const Buffer& data, std::string& payload, WSFrameType& type) {
    WSFrameMetadata metadata;
    if (!decodeFrame(std::string_view(reinterpret_cast<const char*>(data.data()), data.size()), payload, metadata)) {
        return false;
    }
    if (!metadata.final) {
        return false;
    }
    type = metadata.type;
    return true;
}

bool WebSocketProtocol::decodeFrame(std::string_view data, std::string& payload, WSFrameMetadata& metadata) {
    payload.clear();
    if (data.size() < 2) {
        return false;
    }

    metadata = WSFrameMetadata{};
    size_t offset = 0;
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(data.data());
    const std::uint8_t firstByte = bytes[offset++];
    const std::uint8_t secondByte = bytes[offset++];

    const bool fin = (firstByte & 0x80) != 0;
    const std::uint8_t reservedBits = firstByte & 0x70;
    const std::uint8_t opcode = firstByte & 0x0F;
    if (reservedBits != 0 || !frameTypeFromOpcode(opcode, metadata.type)) {
        return false;
    }

    std::uint64_t payloadLength = secondByte & 0x7F;
    if (payloadLength == 126) {
        if (offset + 2 > data.size()) {
            return false;
        }
        payloadLength = (static_cast<std::uint64_t>(bytes[offset]) << 8) |
                        static_cast<std::uint64_t>(bytes[offset + 1]);
        if (payloadLength < 126) {
            return false;
        }
        offset += 2;
    } else if (payloadLength == 127) {
        if (offset + 8 > data.size()) {
            return false;
        }
        payloadLength = 0;
        for (int i = 0; i < 8; ++i) {
            payloadLength = (payloadLength << 8) | bytes[offset++];
        }
        if (payloadLength <= 0xFFFF || (payloadLength & (1ULL << 63)) != 0) {
            return false;
        }
    }

    metadata.final = fin;
    metadata.masked = (secondByte & 0x80) != 0;
    if (isControlOpcode(opcode)) {
        if (!fin || payloadLength > 125) {
            return false;
        }
    }
    if (payloadLength > static_cast<std::uint64_t>((std::numeric_limits<size_t>::max)())) {
        return false;
    }

    std::array<std::uint8_t, 4> mask{};
    if (metadata.masked) {
        if (offset + mask.size() > data.size()) {
            return false;
        }
        std::memcpy(mask.data(), bytes + offset, mask.size());
        offset += mask.size();
    }

    if (offset > data.size() || payloadLength > static_cast<std::uint64_t>(data.size() - offset)) {
        return false;
    }

    const size_t payloadSize = static_cast<size_t>(payloadLength);
    if (!metadata.masked) {
        payload.assign(data.data() + offset, payloadSize);
    } else {
        payload.resize(payloadSize);
        for (size_t i = 0; i < payload.size(); ++i) {
            payload[i] = static_cast<char>(bytes[offset + i] ^ mask[i & 0x03]);
        }
    }

    if (metadata.type == WSFrameType::Close && !isValidClosePayload(payload)) {
        return false;
    }

    return true;
}

bool WebSocketProtocol::decodeFrame(std::string_view data, Buffer& payload, WSFrameMetadata& metadata) {
    payload.clear();
    if (data.size() < 2) {
        return false;
    }

    metadata = WSFrameMetadata{};
    size_t offset = 0;
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(data.data());
    const std::uint8_t firstByte = bytes[offset++];
    const std::uint8_t secondByte = bytes[offset++];

    const bool fin = (firstByte & 0x80) != 0;
    const std::uint8_t reservedBits = firstByte & 0x70;
    const std::uint8_t opcode = firstByte & 0x0F;
    if (reservedBits != 0 || !frameTypeFromOpcode(opcode, metadata.type)) {
        return false;
    }

    std::uint64_t payloadLength = secondByte & 0x7F;
    if (payloadLength == 126) {
        if (offset + 2 > data.size()) {
            return false;
        }
        payloadLength = (static_cast<std::uint64_t>(bytes[offset]) << 8) |
                        static_cast<std::uint64_t>(bytes[offset + 1]);
        if (payloadLength < 126) {
            return false;
        }
        offset += 2;
    } else if (payloadLength == 127) {
        if (offset + 8 > data.size()) {
            return false;
        }
        payloadLength = 0;
        for (int i = 0; i < 8; ++i) {
            payloadLength = (payloadLength << 8) | bytes[offset++];
        }
        if (payloadLength <= 0xFFFF || (payloadLength & (1ULL << 63)) != 0) {
            return false;
        }
    }

    metadata.final = fin;
    metadata.masked = (secondByte & 0x80) != 0;
    if (isControlOpcode(opcode)) {
        if (!fin || payloadLength > 125) {
            return false;
        }
    }
    if (payloadLength > static_cast<std::uint64_t>((std::numeric_limits<size_t>::max)())) {
        return false;
    }

    std::array<std::uint8_t, 4> mask{};
    if (metadata.masked) {
        if (offset + mask.size() > data.size()) {
            return false;
        }
        std::memcpy(mask.data(), bytes + offset, mask.size());
        offset += mask.size();
    }

    if (offset > data.size() || payloadLength > static_cast<std::uint64_t>(data.size() - offset)) {
        return false;
    }

    const size_t payloadSize = static_cast<size_t>(payloadLength);
    payload.resize(payloadSize);
    if (!metadata.masked) {
        if (payloadSize != 0) {
            std::memcpy(payload.data(), bytes + offset, payloadSize);
        }
    } else {
        for (size_t i = 0; i < payload.size(); ++i) {
            payload[i] = static_cast<std::uint8_t>(bytes[offset + i] ^ mask[i & 0x03]);
        }
    }

    if (metadata.type == WSFrameType::Close) {
        const std::string_view closePayload(
            reinterpret_cast<const char*>(payload.data()),
            payload.size());
        if (!isValidClosePayload(closePayload)) {
            return false;
        }
    }

    return true;
}

bool WebSocketProtocol::isControlFrame(WSFrameType type) noexcept {
    return type == WSFrameType::Close || type == WSFrameType::Ping || type == WSFrameType::Pong;
}

bool WebSocketProtocol::isValidClosePayload(std::string_view payload) noexcept {
    if (payload.empty()) {
        return true;
    }
    if (payload.size() == 1) {
        return false;
    }

    const uint16_t code = (static_cast<uint16_t>(static_cast<std::uint8_t>(payload[0])) << 8) |
                          static_cast<uint16_t>(static_cast<std::uint8_t>(payload[1]));
    if (code < 1000) {
        return false;
    }
    if (code == 1004 || code == 1005 || code == 1006 || code == 1015) {
        return false;
    }
    if ((code >= 1016 && code <= 1999) || (code >= 2000 && code <= 2999)) {
        return false;
    }
    return true;
}

} // namespace FastNet
