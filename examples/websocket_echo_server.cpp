#include "FastNet/FastNet.h"

#include <iostream>
#include <string>

int main(int argc, char** argv) {
    const uint16_t port = argc > 1 ? static_cast<uint16_t>(std::stoi(argv[1])) : 8081;

    const FastNet::ErrorCode initCode = FastNet::initialize();
    if (initCode != FastNet::ErrorCode::Success) {
        std::cerr << "FastNet initialize failed: " << FastNet::errorCodeToString(initCode) << '\n';
        return 1;
    }

    auto& ioService = FastNet::getGlobalIoService();
    FastNet::WebSocketServer server(ioService);

    server.setServerErrorCallback([](const FastNet::Error& error) {
        std::cerr << "[ws-echo] " << error.toString() << '\n';
    });

    server.setClientConnectedCallback([](FastNet::ConnectionId clientId, const FastNet::Address& address) {
        std::cout << "client " << clientId << " connected from " << address.toString() << '\n';
    });

    server.setClientDisconnectedCallback(
        [](FastNet::ConnectionId clientId, uint16_t code, const std::string& reason) {
            std::cout << "client " << clientId << " disconnected (" << code << "): " << reason << '\n';
        });

    server.setMessageCallback([&server](FastNet::ConnectionId clientId, const std::string& message) {
        const FastNet::Error result = server.sendTextToClient(clientId, message);
        if (result.isFailure()) {
            std::cerr << "send text failed: " << result.toString() << '\n';
        }
    });

    server.setBinaryCallback([&server](FastNet::ConnectionId clientId, const FastNet::Buffer& data) {
        const FastNet::Error result = server.sendBinaryToClient(clientId, data);
        if (result.isFailure()) {
            std::cerr << "send binary failed: " << result.toString() << '\n';
        }
    });

    const FastNet::Error startResult = server.start(port);
    if (startResult.isFailure()) {
        std::cerr << "server start failed: " << startResult.toString() << '\n';
        FastNet::cleanup();
        return 1;
    }

    std::cout << "WebSocket echo server listening on ws://127.0.0.1:" << port << '\n';
    std::cout << "Press ENTER to stop." << '\n';

    std::string line;
    std::getline(std::cin, line);

    server.stop();
    FastNet::cleanup();
    return 0;
}
