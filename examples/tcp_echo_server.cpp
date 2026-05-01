#include "FastNet/FastNet.h"

#include <iostream>
#include <string>
#include <utility>

int main(int argc, char** argv) {
    const uint16_t port = argc > 1 ? static_cast<uint16_t>(std::stoi(argv[1])) : 9000;

    const FastNet::ErrorCode initCode = FastNet::initialize();
    if (initCode != FastNet::ErrorCode::Success) {
        std::cerr << "FastNet initialize failed: " << FastNet::errorCodeToString(initCode) << '\n';
        return 1;
    }

    auto& ioService = FastNet::getGlobalIoService();
    FastNet::TcpServer server(ioService);

    server.setServerErrorCallback([](const FastNet::Error& error) {
        std::cerr << "[tcp-echo] " << error.toString() << '\n';
    });

    server.setClientConnectedCallback([](FastNet::ConnectionId clientId, const FastNet::Address& address) {
        std::cout << "client " << clientId << " connected from " << address.toString() << '\n';
    });

    server.setClientDisconnectedCallback([](FastNet::ConnectionId clientId, const std::string& reason) {
        std::cout << "client " << clientId << " disconnected: " << reason << '\n';
    });

    server.setOwnedDataReceivedCallback([&server](FastNet::ConnectionId clientId, FastNet::Buffer&& data) {
        const FastNet::Error result = server.sendToClient(clientId, std::move(data));
        if (result.isFailure()) {
            std::cerr << "echo send failed: " << result.toString() << '\n';
        }
    });

    const FastNet::Error startResult = server.start(port);
    if (startResult.isFailure()) {
        std::cerr << "server start failed: " << startResult.toString() << '\n';
        FastNet::cleanup();
        return 1;
    }

    std::cout << "TCP echo server listening on 0.0.0.0:" << port << '\n';
    std::cout << "Press ENTER to stop." << '\n';

    std::string line;
    std::getline(std::cin, line);

    server.stop();
    FastNet::cleanup();
    return 0;
}
