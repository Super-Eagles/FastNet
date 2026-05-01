#include "FastNet/FastNet.h"

#include <future>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    const std::string url = argc > 1 ? argv[1] : "http://127.0.0.1:8080/healthz";

    const FastNet::ErrorCode initCode = FastNet::initialize();
    if (initCode != FastNet::ErrorCode::Success) {
        std::cerr << "FastNet initialize failed: " << FastNet::errorCodeToString(initCode) << '\n';
        return 1;
    }

    auto& ioService = FastNet::getGlobalIoService();
    FastNet::HttpClient client(ioService);
    client.setConnectTimeout(5000);
    client.setRequestTimeout(5000);
    client.setReadTimeout(5000);

    std::promise<int> done;
    auto future = done.get_future();

    const bool connectStarted = client.connect(url, [&](bool success, const std::string& errorMessage) {
        if (!success) {
            std::cerr << "connect failed: " << errorMessage << '\n';
            done.set_value(1);
            return;
        }

        if (!client.get("", {}, [&](const FastNet::HttpResponse& response) {
                std::cout << "HTTP " << response.statusCode << ' ' << response.statusMessage << '\n';
                for (const auto& header : response.headers) {
                    std::cout << header.first << ": " << header.second << '\n';
                }
                std::cout << '\n' << response.body << '\n';
                done.set_value(response.statusCode == 200 ? 0 : 2);
            })) {
            std::cerr << "request failed: " << client.getLastError().toString() << '\n';
            done.set_value(1);
        }
    });

    if (!connectStarted) {
        std::cerr << "connect start failed: " << client.getLastError().toString() << '\n';
        FastNet::cleanup();
        return 1;
    }

    const int exitCode = future.get();
    client.disconnect();
    FastNet::cleanup();
    return exitCode;
}
