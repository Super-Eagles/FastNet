#include "FastNet/FastNet.h"
#include "TestSupport.h"

#include <iostream>

int main() {
    const FastNet::Error error(FastNet::ErrorCode::InvalidArgument, "bad input");
    FASTNET_TEST_ASSERT(error.isFailure());
    FASTNET_TEST_ASSERT_EQ(error.getCode(), FastNet::ErrorCode::InvalidArgument);
    FASTNET_TEST_ASSERT_MSG(error.toString().find("bad input") != std::string::npos, "error text should include message");

    auto result = FastNet::Result<int>::success(42);
    FASTNET_TEST_ASSERT(result.isSuccess());
    FASTNET_TEST_ASSERT_EQ(result.value(), 42);

    auto failure = FastNet::Result<int>::error(FastNet::ErrorCode::ConnectionError, "offline");
    FASTNET_TEST_ASSERT(failure.isError());
    FASTNET_TEST_ASSERT_EQ(failure.errorCode(), FastNet::ErrorCode::ConnectionError);
    FASTNET_TEST_ASSERT_MSG(failure.errorMessage().find("offline") != std::string::npos, "failure should keep error text");

    auto voidFailure = FastNet::Result<void>::error(FastNet::ErrorCode::TimeoutError, "slow");
    FASTNET_TEST_ASSERT(voidFailure.isError());
    FASTNET_TEST_ASSERT_EQ(voidFailure.errorCode(), FastNet::ErrorCode::TimeoutError);

    auto& policy = FastNet::ExceptionPolicy::getInstance();
    policy.disableExceptions();
    FastNet::handleError(FastNet::Error::success());

    std::cout << "error model tests passed" << '\n';
    return 0;
}
