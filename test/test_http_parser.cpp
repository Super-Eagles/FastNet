#include "FastNet/FastNet.h"
#include "TestSupport.h"

#include <cstdint>
#include <iostream>
#include <string>

namespace {

std::string makeHeaderFlood() {
    std::string request = "GET / HTTP/1.1\r\n";
    for (size_t index = 0; index < 129; ++index) {
        request += "X-Test-";
        request += std::to_string(index);
        request += ": value\r\n";
    }
    request += "\r\n";
    return request;
}

void runParserFuzzSmoke() {
    uint32_t state = 0xF357A11u;
    for (size_t round = 0; round < 512; ++round) {
        state = state * 1103515245u + 12345u;
        std::string fuzz((state >> 24) & 0x7Fu, '\0');
        for (char& ch : fuzz) {
            state = state * 1103515245u + 12345u;
            ch = static_cast<char>((state >> 24) & 0xFFu);
        }

        FastNet::HttpRequestView requestView;
        FastNet::HttpParser::parseRequestHead(fuzz, requestView);
        FastNet::HttpParser::parseRequest(fuzz, requestView);

        FastNet::HttpResponseView responseView;
        FastNet::HttpParser::parseResponseHead(fuzz, responseView);
        FastNet::HttpParser::parseResponse(fuzz, responseView);

        std::string chunkBody;
        FastNet::HttpParser::parseChunked(fuzz, chunkBody);
    }
}

} // namespace

int main() {
    const std::string request =
        "POST /api/items?id=7&name=fastnet HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Length: 5\r\n"
        "Connection: keep-alive\r\n"
        "\r\n"
        "hello";

    FastNet::HttpRequestView requestView;
    FASTNET_TEST_ASSERT(FastNet::HttpParser::parseRequest(request, requestView));
    FASTNET_TEST_ASSERT_EQ(requestView.method, std::string_view("POST"));
    FASTNET_TEST_ASSERT_EQ(requestView.uri, std::string_view("/api/items"));
    FASTNET_TEST_ASSERT_EQ(requestView.queryParams["id"], std::string_view("7"));
    FASTNET_TEST_ASSERT_EQ(requestView.queryParams["name"], std::string_view("fastnet"));
    FASTNET_TEST_ASSERT_EQ(requestView.body, std::string_view("hello"));
    FASTNET_TEST_ASSERT(requestView.isKeepAlive());

    FastNet::HttpRequestView requestHeadView;
    const std::string requestHead = request.substr(0, request.find("\r\n\r\n") + 4);
    FASTNET_TEST_ASSERT(FastNet::HttpParser::parseRequestHead(requestHead, requestHeadView));
    FASTNET_TEST_ASSERT_EQ(requestHeadView.method, std::string_view("POST"));
    FASTNET_TEST_ASSERT_EQ(requestHeadView.uri, std::string_view("/api/items"));
    FASTNET_TEST_ASSERT_EQ(requestHeadView.getContentLength(), static_cast<size_t>(5));
    FASTNET_TEST_ASSERT(requestHeadView.body.empty());

    const std::string response =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 5\r\n"
        "Connection: close\r\n"
        "\r\n"
        "world";

    FastNet::HttpResponseView responseView;
    FASTNET_TEST_ASSERT(FastNet::HttpParser::parseResponse(response, responseView));
    FASTNET_TEST_ASSERT_EQ(responseView.statusCode, 200);
    FASTNET_TEST_ASSERT_EQ(responseView.body, std::string_view("world"));
    FASTNET_TEST_ASSERT(!responseView.isKeepAlive());

    FastNet::HttpResponseView responseHeadView;
    const std::string responseHead = response.substr(0, response.find("\r\n\r\n") + 4);
    FASTNET_TEST_ASSERT(FastNet::HttpParser::parseResponseHead(responseHead, responseHeadView));
    FASTNET_TEST_ASSERT_EQ(responseHeadView.statusCode, 200);
    FASTNET_TEST_ASSERT_EQ(responseHeadView.getContentLength(), static_cast<size_t>(5));
    FASTNET_TEST_ASSERT(responseHeadView.body.empty());

    const std::string chunked = FastNet::HttpParser::buildChunked("hello");
    std::string decodedChunked;
    FASTNET_TEST_ASSERT(FastNet::HttpParser::parseChunked(chunked, decodedChunked));
    FASTNET_TEST_ASSERT_EQ(decodedChunked, "hello");
    FASTNET_TEST_ASSERT_EQ(FastNet::HttpParser::urlDecode("a%20b%2Fc"), "a b/c");
    FASTNET_TEST_ASSERT_EQ(FastNet::HttpParser::urlEncode("a b/c"), "a%20b%2Fc");

    FastNet::HttpRequestView invalidRequest;
    FASTNET_TEST_ASSERT(!FastNet::HttpParser::parseRequest(
        "POST / HTTP/1.1\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\nx",
        invalidRequest));
    FASTNET_TEST_ASSERT(invalidRequest.headers.empty());
    FASTNET_TEST_ASSERT(!FastNet::HttpParser::parseRequest(
        "POST / HTTP/1.1\r\nContent-Length: 1\r\nTransfer-Encoding: chunked\r\n\r\nx",
        invalidRequest));
    FASTNET_TEST_ASSERT(!FastNet::HttpParser::parseRequestHead(
        "GE T / HTTP/1.1\r\nHost: localhost\r\n\r\n",
        invalidRequest));
    FASTNET_TEST_ASSERT(!FastNet::HttpParser::parseRequestHead(
        "GET / HTTP/1.1\r\nHost: localhost\r\n folded: value\r\n\r\n",
        invalidRequest));
    FASTNET_TEST_ASSERT(!FastNet::HttpParser::parseRequestHead(makeHeaderFlood(), invalidRequest));
    FASTNET_TEST_ASSERT(!FastNet::HttpParser::parseRequest(
        "POST / HTTP/1.1\r\nContent-Length: 5\r\n\r\nhi",
        invalidRequest));

    FastNet::HttpResponseView invalidResponse;
    FASTNET_TEST_ASSERT(!FastNet::HttpParser::parseResponseHead(
        "HTTP/1.1 two OK\r\nContent-Length: 0\r\n\r\n",
        invalidResponse));
    FASTNET_TEST_ASSERT(!FastNet::HttpParser::parseResponse(
        "HTTP/1.1 200 OK\r\nContent-Length: 1\r\nTransfer-Encoding: chunked\r\n\r\nx",
        invalidResponse));

    FASTNET_TEST_ASSERT(!FastNet::HttpParser::parseChunked("5\r\nhel\r\n0\r\n\r\n", decodedChunked));
    FASTNET_TEST_ASSERT(!FastNet::HttpParser::parseChunked(
        "FFFFFFFFFFFFFFFFF\r\nx\r\n0\r\n\r\n",
        decodedChunked));
    FASTNET_TEST_ASSERT(!FastNet::HttpParser::parseChunked("0\r\n bad\r\n\r\n", decodedChunked));

    runParserFuzzSmoke();

    std::cout << "http parser tests passed" << '\n';
    return 0;
}
