#include "FastNet/FastNet.h"
#include "TestSupport.h"

#include <iostream>
#include <string>

int main() {
    FastNet::HttpMultipartBuilder multipart("FastNetBoundaryForTest");
    multipart.addField("prompt", "hello")
        .addFile("attachment", "note.txt", "file-body", "text/plain");

    const FastNet::HttpHeaders headers = multipart.headers();
    const std::string body = multipart.build();

    FASTNET_TEST_ASSERT_EQ(headers.at("Content-Type"),
                           "multipart/form-data; boundary=FastNetBoundaryForTest");
    FASTNET_TEST_ASSERT(body.find("--FastNetBoundaryForTest\r\n") != std::string::npos);
    FASTNET_TEST_ASSERT(body.find("Content-Disposition: form-data; name=\"prompt\"") != std::string::npos);
    FASTNET_TEST_ASSERT(body.find("\r\n\r\nhello\r\n") != std::string::npos);
    FASTNET_TEST_ASSERT(
        body.find("Content-Disposition: form-data; name=\"attachment\"; filename=\"note.txt\"") !=
        std::string::npos);
    FASTNET_TEST_ASSERT(body.find("Content-Type: text/plain\r\n") != std::string::npos);
    FASTNET_TEST_ASSERT(body.find("\r\n\r\nfile-body\r\n") != std::string::npos);
    const std::string trailer = "--FastNetBoundaryForTest--\r\n";
    FASTNET_TEST_ASSERT(body.rfind(trailer) == body.size() - trailer.size());

    std::cout << "http utilities tests passed" << '\n';
    return 0;
}
