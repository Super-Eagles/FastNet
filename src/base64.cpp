/**
 * @file base64.cpp
 * @brief Base64 helpers for FastNet
 */
#include "base64.h"

#include <array>
#include <cctype>
#include <cstdint>

namespace FastNet {

namespace {

constexpr char kBase64Alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz"
    "0123456789+/";

const std::array<int8_t, 256>& base64DecodeTable() {
    static const std::array<int8_t, 256> table = [] {
        std::array<int8_t, 256> values{};
        values.fill(-1);
        for (int index = 0; index < 64; ++index) {
            values[static_cast<unsigned char>(kBase64Alphabet[index])] = static_cast<int8_t>(index);
        }
        return values;
    }();
    return table;
}

bool isIgnoredWhitespace(unsigned char ch) {
    return std::isspace(ch) != 0;
}

std::string encodeBytes(const uint8_t* data, size_t size) {
    std::string encoded;
    encoded.reserve(((size + 2) / 3) * 4);

    size_t offset = 0;
    while (offset < size) {
        const size_t remaining = size - offset;
        const uint32_t octetA = data[offset++];
        const uint32_t octetB = remaining > 1 ? data[offset++] : 0;
        const uint32_t octetC = remaining > 2 ? data[offset++] : 0;
        const uint32_t triple = (octetA << 16) | (octetB << 8) | octetC;

        encoded.push_back(kBase64Alphabet[(triple >> 18) & 0x3F]);
        encoded.push_back(kBase64Alphabet[(triple >> 12) & 0x3F]);
        encoded.push_back(remaining > 1 ? kBase64Alphabet[(triple >> 6) & 0x3F] : '=');
        encoded.push_back(remaining > 2 ? kBase64Alphabet[triple & 0x3F] : '=');
    }

    return encoded;
}

bool normalizeBase64(std::string_view encoded, std::string& normalized) {
    normalized.clear();
    normalized.reserve(encoded.size());

    const auto& table = base64DecodeTable();
    for (char ch : encoded) {
        const unsigned char byte = static_cast<unsigned char>(ch);
        if (isIgnoredWhitespace(byte)) {
            continue;
        }
        if (ch != '=' && table[byte] < 0) {
            return false;
        }
        normalized.push_back(ch);
    }

    return true;
}

} // namespace

std::string base64Encode(const Buffer& data) {
    if (data.empty()) {
        return {};
    }
    return encodeBytes(data.data(), data.size());
}

std::string base64Encode(std::string_view data) {
    if (data.empty()) {
        return {};
    }
    return encodeBytes(reinterpret_cast<const uint8_t*>(data.data()), data.size());
}

bool tryBase64Decode(std::string_view encoded, Buffer& output) {
    output.clear();

    std::string normalized;
    if (!normalizeBase64(encoded, normalized)) {
        return false;
    }

    if (normalized.empty()) {
        return true;
    }
    if ((normalized.size() % 4) != 0) {
        return false;
    }

    size_t padding = 0;
    if (!normalized.empty() && normalized.back() == '=') {
        ++padding;
        if (normalized.size() >= 2 && normalized[normalized.size() - 2] == '=') {
            ++padding;
        }
    }

    const size_t quartetCount = normalized.size() / 4;
    output.reserve((quartetCount * 3) - padding);

    const auto& table = base64DecodeTable();
    for (size_t quartet = 0; quartet < quartetCount; ++quartet) {
        const bool isLastQuartet = (quartet + 1) == quartetCount;
        const size_t baseIndex = quartet * 4;

        const char c0 = normalized[baseIndex];
        const char c1 = normalized[baseIndex + 1];
        const char c2 = normalized[baseIndex + 2];
        const char c3 = normalized[baseIndex + 3];

        if (c0 == '=' || c1 == '=') {
            return false;
        }
        if (!isLastQuartet && (c2 == '=' || c3 == '=')) {
            return false;
        }

        const int v0 = table[static_cast<unsigned char>(c0)];
        const int v1 = table[static_cast<unsigned char>(c1)];
        if (v0 < 0 || v1 < 0) {
            return false;
        }

        if (c2 == '=') {
            if (!isLastQuartet || c3 != '=') {
                return false;
            }
            output.push_back(static_cast<uint8_t>((v0 << 2) | (v1 >> 4)));
            continue;
        }

        const int v2 = table[static_cast<unsigned char>(c2)];
        if (v2 < 0) {
            return false;
        }

        output.push_back(static_cast<uint8_t>((v0 << 2) | (v1 >> 4)));
        output.push_back(static_cast<uint8_t>(((v1 & 0x0F) << 4) | (v2 >> 2)));

        if (c3 == '=') {
            if (!isLastQuartet) {
                return false;
            }
            continue;
        }

        const int v3 = table[static_cast<unsigned char>(c3)];
        if (v3 < 0) {
            return false;
        }

        output.push_back(static_cast<uint8_t>(((v2 & 0x03) << 6) | v3));
    }

    return true;
}

bool tryBase64DecodeToString(std::string_view encoded, std::string& output) {
    Buffer buffer;
    if (!tryBase64Decode(encoded, buffer)) {
        output.clear();
        return false;
    }

    if (buffer.empty()) {
        output.clear();
        return true;
    }

    output.assign(reinterpret_cast<const char*>(buffer.data()), buffer.size());
    return true;
}

Buffer base64Decode(std::string_view encoded) {
    Buffer output;
    if (!tryBase64Decode(encoded, output)) {
        output.clear();
    }
    return output;
}

std::string base64DecodeToString(std::string_view encoded) {
    std::string output;
    if (!tryBase64DecodeToString(encoded, output)) {
        output.clear();
    }
    return output;
}

} // namespace FastNet
