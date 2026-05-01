#include "FastNet/FastNet.h"
#include "TestSupport.h"

#include <iostream>
#include <string>

int main() {
    const std::string plain = "hello fastnet";
    const std::string encoded = FastNet::base64Encode(plain);
    FASTNET_TEST_ASSERT_EQ(encoded, "aGVsbG8gZmFzdG5ldA==");
    FASTNET_TEST_ASSERT_EQ(FastNet::base64DecodeToString(encoded), plain);

    FastNet::Buffer decoded;
    FASTNET_TEST_ASSERT(FastNet::tryBase64Decode(encoded, decoded));
    FASTNET_TEST_ASSERT_EQ(std::string(decoded.begin(), decoded.end()), plain);

    std::string invalidDecoded;
    FASTNET_TEST_ASSERT(!FastNet::tryBase64DecodeToString("not base64!", invalidDecoded));

    std::cout << "base64 tests passed" << '\n';
    return 0;
}
