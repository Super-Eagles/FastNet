#include "FastNet/FastNet.h"
#include "TestSupport.h"

#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <string>

namespace {

struct UdpState {
    std::mutex mutex;
    std::condition_variable condition;
    bool received = false;
    bool error = false;
    FastNet::Address sender;
    FastNet::Buffer payload;
    std::string errorMessage;
};

} // namespace

int main() {
    using namespace std::chrono_literals;

    FASTNET_TEST_ASSERT_EQ(FastNet::initialize(2), FastNet::ErrorCode::Success);
    struct CleanupGuard {
        ~CleanupGuard() {
            FastNet::cleanup();
        }
    } cleanupGuard;

    auto& ioService = FastNet::getGlobalIoService();
    FastNet::UdpSocket receiver(ioService);
    FastNet::UdpSocket sender(ioService);

    UdpState state;
    receiver.setReceiveBufferSize(4096);
    receiver.setSendBufferSize(4096);
    receiver.setBroadcast(false);
    receiver.setErrorCallback([&](FastNet::ErrorCode, const std::string& message) {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.error = true;
        state.errorMessage = message;
        state.condition.notify_all();
    });
    receiver.setDataReceivedCallback([&](const FastNet::Address& senderAddress, const FastNet::Buffer& data) {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.received = true;
        state.sender = senderAddress;
        state.payload = data;
        state.condition.notify_all();
    });

    FASTNET_TEST_ASSERT(!sender.sendTo(FastNet::Address("127.0.0.1", 9), std::string_view("not-bound")));
    FASTNET_TEST_ASSERT(receiver.bind(0, "127.0.0.1"));
    FASTNET_TEST_ASSERT(receiver.isBound());
    FASTNET_TEST_ASSERT(receiver.getLocalAddress().port != 0);
    FASTNET_TEST_ASSERT(receiver.startReceive());
    FASTNET_TEST_ASSERT(receiver.isReceiving());
    FASTNET_TEST_ASSERT(!receiver.startReceive());

    sender.setBroadcast(false);
    FASTNET_TEST_ASSERT(sender.bind(0, "127.0.0.1"));
    FASTNET_TEST_ASSERT(sender.getLocalAddress().port != 0);

    const std::string message = "udp-loopback-payload";
    FASTNET_TEST_ASSERT(sender.sendTo(receiver.getLocalAddress(), std::string_view(message)));

    {
        std::unique_lock<std::mutex> lock(state.mutex);
        FASTNET_TEST_ASSERT_MSG(
            state.condition.wait_for(lock, 2s, [&]() { return state.received || state.error; }),
            "Timed out waiting for UDP loopback packet");
        FASTNET_TEST_ASSERT_MSG(!state.error, state.errorMessage);
        FASTNET_TEST_ASSERT(state.received);
        FASTNET_TEST_ASSERT_EQ(std::string(state.payload.begin(), state.payload.end()), message);
        FASTNET_TEST_ASSERT_EQ(state.sender.port, sender.getLocalAddress().port);
    }

    receiver.stopReceive();
    FASTNET_TEST_ASSERT(!receiver.isReceiving());

    std::cout << "udp socket tests passed" << '\n';
    return 0;
}
