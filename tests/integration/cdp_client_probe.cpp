#include "prowsetk/cdp.hpp"
#include <iostream>
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    try {
        auto network = prowsetk::make_socket_network_client();
        prowsetk::HttpRequest request;
        request.url = argv[1]; request.timeout_ms = 2000; request.max_response_bytes = 4096;
        prowsetk::CdpClient client(network->open_websocket(request));
        const auto result = client.call("Browser.getVersion");
        if (result != "{\"product\":\"fixture\"}") return 3;
        if (client.take_events().size() != 1) return 4;
        return 0;
    } catch (...) { std::cerr << "CDP failed\n"; return 1; }
}
