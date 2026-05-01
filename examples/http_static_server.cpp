#include "FastNet/FastNet.h"

#include <iostream>
#include <string>

int main(int argc, char** argv) {
    const uint16_t port = argc > 1 ? static_cast<uint16_t>(std::stoi(argv[1])) : 8080;
    const std::string staticRoot = argc > 2 ? argv[2] : ".";

    const FastNet::ErrorCode initCode = FastNet::initialize();
    if (initCode != FastNet::ErrorCode::Success) {
        std::cerr << "FastNet initialize failed: " << FastNet::errorCodeToString(initCode) << '\n';
        return 1;
    }

    auto& ioService = FastNet::getGlobalIoService();
    FastNet::HttpServer server(ioService);

    server.setServerErrorCallback([](const FastNet::Error& error) {
        std::cerr << "[http-static] " << error.toString() << '\n';
    });

    server.registerGet("/", [](const FastNet::HttpRequest&, FastNet::HttpResponse& response) {
        response.statusCode = 200;
        response.statusMessage = "OK";
        response.headers["Content-Type"] = "text/html; charset=utf-8";
        response.body =
            "<html><body><h1>FastNet HTTP Server</h1>"
            "<p>Try <code>/healthz</code> or <code>/static/...</code>.</p>"
            "</body></html>";
    });

    server.registerGet("/healthz", [](const FastNet::HttpRequest&, FastNet::HttpResponse& response) {
        response.statusCode = 200;
        response.statusMessage = "OK";
        response.headers["Content-Type"] = "text/plain; charset=utf-8";
        response.body = "ok";
    });

    server.registerPost("/api/echo", [](const FastNet::HttpRequest& request, FastNet::HttpResponse& response) {
        response.statusCode = 200;
        response.statusMessage = "OK";
        response.headers["Content-Type"] = "application/json; charset=utf-8";
        response.body = request.body;
    });

    server.registerStaticFileHandler("/static", staticRoot);
    server.setMaxRequestSize(2 * 1024 * 1024);
    server.setStaticFileCacheLimit(8 * 1024 * 1024);

    const FastNet::Error startResult = server.start(port);
    if (startResult.isFailure()) {
        std::cerr << "server start failed: " << startResult.toString() << '\n';
        FastNet::cleanup();
        return 1;
    }

    std::cout << "HTTP server listening on http://127.0.0.1:" << port << '\n';
    std::cout << "Static root mapped to /static -> " << staticRoot << '\n';
    std::cout << "Press ENTER to stop." << '\n';

    std::string line;
    std::getline(std::cin, line);

    server.stop();
    FastNet::cleanup();
    return 0;
}
