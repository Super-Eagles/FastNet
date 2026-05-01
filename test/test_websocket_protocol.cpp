#include "FastNet/FastNet.h"
#include "TestSupport.h"

#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <string>

namespace {

bool decodeStringFrame(const std::string& frame) {
    std::string payload;
    FastNet::WSFrameMetadata metadata;
    return FastNet::WebSocketProtocol::decodeFrame(frame, payload, metadata);
}

std::string frameFromBytes(std::initializer_list<unsigned int> bytes) {
    std::string frame;
    frame.reserve(bytes.size());
    for (unsigned int byte : bytes) {
        frame.push_back(static_cast<char>(byte & 0xFFu));
    }
    return frame;
}

std::string extendedFrame(unsigned int lengthMarker, uint64_t length, size_t payloadSize) {
    std::string frame;
    frame.push_back(static_cast<char>(0x81));
    frame.push_back(static_cast<char>(lengthMarker));
    if (lengthMarker == 126) {
        frame.push_back(static_cast<char>((length >> 8) & 0xFFu));
        frame.push_back(static_cast<char>(length & 0xFFu));
    } else {
        for (int shift = 56; shift >= 0; shift -= 8) {
            frame.push_back(static_cast<char>((length >> shift) & 0xFFu));
        }
    }
    frame.append(payloadSize, 'x');
    return frame;
}

} // namespace

int main() {
    const std::string accept =
        FastNet::WebSocketProtocol::createAcceptKey("dGhlIHNhbXBsZSBub25jZQ==");
    FASTNET_TEST_ASSERT_EQ(accept, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");

    const std::string frame =
        FastNet::WebSocketProtocol::encodeFrame("hello", FastNet::WSFrameType::Text, true);

    std::string payload;
    FastNet::WSFrameMetadata metadata;
    FASTNET_TEST_ASSERT(FastNet::WebSocketProtocol::decodeFrame(frame, payload, metadata));
    FASTNET_TEST_ASSERT_EQ(payload, "hello");
    FASTNET_TEST_ASSERT_EQ(metadata.type, FastNet::WSFrameType::Text);
    FASTNET_TEST_ASSERT(metadata.final);
    FASTNET_TEST_ASSERT(metadata.masked);

    const FastNet::Buffer binaryInput = {0x00, 0x11, 0x22, 0x33, 0x44};
    const std::string binaryFrame =
        FastNet::WebSocketProtocol::encodeFrame(binaryInput, FastNet::WSFrameType::Binary, true);
    FastNet::Buffer binaryPayload;
    FastNet::WSFrameMetadata binaryMetadata;
    FASTNET_TEST_ASSERT(FastNet::WebSocketProtocol::decodeFrame(binaryFrame, binaryPayload, binaryMetadata));
    FASTNET_TEST_ASSERT_EQ(binaryPayload.size(), binaryInput.size());
    FASTNET_TEST_ASSERT(binaryPayload == binaryInput);
    FASTNET_TEST_ASSERT_EQ(binaryMetadata.type, FastNet::WSFrameType::Binary);
    FASTNET_TEST_ASSERT(binaryMetadata.final);
    FASTNET_TEST_ASSERT(binaryMetadata.masked);

    FASTNET_TEST_ASSERT(FastNet::WebSocketProtocol::isControlFrame(FastNet::WSFrameType::Ping));
    FASTNET_TEST_ASSERT(FastNet::WebSocketProtocol::isValidClosePayload(std::string("\x03\xE8", 2)));
    FASTNET_TEST_ASSERT(!FastNet::WebSocketProtocol::isValidClosePayload(std::string("\x03", 1)));

    FASTNET_TEST_ASSERT(!decodeStringFrame(frameFromBytes({0xC1, 0x00})));
    FASTNET_TEST_ASSERT(!decodeStringFrame(frameFromBytes({0x83, 0x00})));
    FASTNET_TEST_ASSERT(!decodeStringFrame(frameFromBytes({0x09, 0x00})));
    FASTNET_TEST_ASSERT(!decodeStringFrame(extendedFrame(126, 5, 5)));
    FASTNET_TEST_ASSERT(!decodeStringFrame(extendedFrame(127, 5, 5)));
    FASTNET_TEST_ASSERT(!decodeStringFrame(extendedFrame(127, 0x8000000000000000ULL, 0)));

    std::string oversizedControl = frameFromBytes({0x89, 126, 0x00, 0x7E});
    oversizedControl.append(126, 'x');
    FASTNET_TEST_ASSERT(!decodeStringFrame(oversizedControl));

    uint32_t fuzzState = 0xC001D00Du;
    for (size_t round = 0; round < 512; ++round) {
        fuzzState = fuzzState * 1664525u + 1013904223u;
        std::string fuzz((fuzzState >> 24) & 0x3Fu, '\0');
        for (char& ch : fuzz) {
            fuzzState = fuzzState * 1664525u + 1013904223u;
            ch = static_cast<char>((fuzzState >> 24) & 0xFFu);
        }

        std::string fuzzPayload;
        FastNet::WSFrameMetadata fuzzMetadata;
        FastNet::WebSocketProtocol::decodeFrame(fuzz, fuzzPayload, fuzzMetadata);

        FastNet::Buffer fuzzBufferPayload;
        FastNet::WebSocketProtocol::decodeFrame(fuzz, fuzzBufferPayload, fuzzMetadata);
    }

    std::cout << "websocket protocol tests passed" << '\n';
    return 0;
}
