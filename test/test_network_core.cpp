#include "FastNet/FastNet.h"
#include "TestSupport.h"

#include <iostream>

int main() {
    FASTNET_TEST_ASSERT(FastNet::Address::isValidIPv6("1:2:3:4:5:6:192.168.0.1"));
    FASTNET_TEST_ASSERT(FastNet::Address::isValidIPv6("[::ffff:192.168.0.1]"));
    FASTNET_TEST_ASSERT(!FastNet::Address::isValidIPv6("1:2:3:4:5:6:7:8::"));

    const auto parsed = FastNet::Address::parse("[1:2:3:4:5:6:192.168.0.1]:443");
    FASTNET_TEST_ASSERT(parsed.has_value());
    FASTNET_TEST_ASSERT_EQ(parsed->port, static_cast<uint16_t>(443));

    FastNet::FastBuffer buffer;
    buffer.append("abc");
    buffer.append(buffer.data(), buffer.size());
    FASTNET_TEST_ASSERT_EQ(buffer.toString(), "abcabc");

    FastNet::MemoryPool<64> firstPool(1);
    FastNet::MemoryPool<64> secondPool(1);
    void* firstBlock = firstPool.allocate();
    FASTNET_TEST_ASSERT(firstBlock != nullptr);
    firstPool.deallocate(firstBlock);

    void* secondBlock = secondPool.allocate();
    FASTNET_TEST_ASSERT(secondBlock != nullptr);
    FASTNET_TEST_ASSERT_MSG(secondBlock != firstBlock, "thread-local caches must be isolated per pool instance");
    secondPool.deallocate(secondBlock);

    const auto firstStats = firstPool.getStats();
    const auto secondStats = secondPool.getStats();
    FASTNET_TEST_ASSERT_EQ(firstStats.allocated, static_cast<size_t>(0));
    FASTNET_TEST_ASSERT_EQ(secondStats.allocated, static_cast<size_t>(0));
    FASTNET_TEST_ASSERT(firstStats.total >= 1);
    FASTNET_TEST_ASSERT(secondStats.total >= 1);

    std::cout << "network core tests passed" << '\n';
    return 0;
}
