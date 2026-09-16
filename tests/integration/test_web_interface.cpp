#include <gtest/gtest.h>

#if !defined(_WIN32)

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <string>
#include <thread>

#include "prowsetk/web_interface.hpp"

using prowsetk::HttpServer;
using prowsetk::WebInterface;
using prowsetk::WebInterfaceConfig;

namespace {

// A minimal blocking HTTP/1.1 client for loopback-only tests. It is hermetic:
// it never leaves the machine.
std::string raw_request(std::uint16_t port, const std::string& request) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return {};
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&address),
                  sizeof address) != 0) {
        ::close(fd);
        return {};
    }
    ::send(fd, request.data(), request.size(), 0);
    ::shutdown(fd, SHUT_WR);

    std::string response;
    char buffer[4096];
    ssize_t count = 0;
    while ((count = ::recv(fd, buffer, sizeof buffer, 0)) > 0) {
        response.append(buffer, static_cast<std::size_t>(count));
    }
    ::close(fd);
    return response;
}

std::string status_line(const std::string& response) {
    const std::size_t end = response.find("\r\n");
    return response.substr(0, end);
}

}  // namespace

TEST(WebInterfaceServe, ServesHealthOverLoopbackSocket) {
    WebInterfaceConfig config;
    config.browser.javascript = false;
    WebInterface interface(std::move(config));

    HttpServer server(interface, "127.0.0.1", 0);
    ASSERT_NE(server.port(), 0);

    std::thread runner([&] { server.run(); });

    const std::string response =
        raw_request(server.port(), "GET /api/health HTTP/1.1\r\n"
                                   "Host: 127.0.0.1\r\n"
                                   "Connection: close\r\n\r\n");

    server.stop();
    runner.join();

    EXPECT_EQ(status_line(response), "HTTP/1.1 200 OK");
    EXPECT_NE(response.find("\"engine\":\"Flatworm\""), std::string::npos);
}

TEST(WebInterfaceServe, CreatesSessionOverLoopbackSocket) {
    WebInterfaceConfig config;
    config.browser.javascript = false;
    WebInterface interface(std::move(config));

    HttpServer server(interface, "127.0.0.1", 0);
    std::thread runner([&] { server.run(); });

    const std::string body = "{}";
    const std::string request =
        "POST /api/sessions HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: " +
        std::to_string(body.size()) + "\r\n"
        "Connection: close\r\n\r\n" + body;

    const std::string response = raw_request(server.port(), request);

    server.stop();
    runner.join();

    EXPECT_EQ(status_line(response), "HTTP/1.1 201 Created");
    EXPECT_NE(response.find("\"id\":\"session-1\""), std::string::npos);
}

#endif  // !defined(_WIN32)
