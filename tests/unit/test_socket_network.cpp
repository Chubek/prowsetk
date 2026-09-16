#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "prowsetk/error.hpp"
#include "prowsetk/network_client.hpp"

#if defined(__unix__) || defined(__APPLE__)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#if defined(__unix__) || defined(__APPLE__)
namespace {

using prowsetk::Error;
using prowsetk::HttpRequest;
using prowsetk::HttpResponse;

// A minimal loopback HTTP server used to exercise the real socket client. The
// responder receives the raw request and returns the raw response bytes.
class LoopbackServer {
public:
    explicit LoopbackServer(std::function<std::string(const std::string&)> responder)
        : responder_(std::move(responder)) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        EXPECT_GE(fd_, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        EXPECT_EQ(::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)),
                  0);
        socklen_t length = sizeof(addr);
        EXPECT_EQ(::getsockname(fd_, reinterpret_cast<sockaddr*>(&addr),
                                &length), 0);
        port_ = ntohs(addr.sin_port);
        EXPECT_EQ(::listen(fd_, 4), 0);
        thread_ = std::thread([this] { serve(); });
    }

    ~LoopbackServer() { stop(); }

    void stop() {
        running_ = false;
        wake_.notify_all();
        if (thread_.joinable()) {
            thread_.join();
        }
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    std::string url(const std::string& path = "/") const {
        return "http://127.0.0.1:" + std::to_string(port_) + path;
    }

    const std::string& last_request() const { return last_request_; }

private:
    void serve() {
        while (running_) {
            // Poll with a short timeout so `stop()` can join promptly even
            // while the server is waiting for a connection.
            pollfd listen_poll{fd_, POLLIN, 0};
            const int poll_rc = ::poll(&listen_poll, 1, 100);
            if (poll_rc <= 0) {
                continue;
            }
            sockaddr_in client{};
            socklen_t client_length = sizeof(client);
            const int client_fd =
                ::accept(fd_, reinterpret_cast<sockaddr*>(&client),
                         &client_length);
            if (client_fd < 0) {
                continue;
            }
            std::string request;
            char buffer[4096];
            while (request.find("\r\n\r\n") == std::string::npos && running_) {
                const ssize_t n = ::recv(client_fd, buffer, sizeof(buffer), 0);
                if (n <= 0) {
                    break;
                }
                request.append(buffer, static_cast<std::size_t>(n));
            }
            last_request_ = request;
            {
                std::unique_lock<std::mutex> lock(wake_mutex_);
                wake_.wait_for(lock, std::chrono::milliseconds(50),
                               [this] { return !running_; });
            }
            if (!running_) {
                ::close(client_fd);
                continue;
            }
            std::string response;
            try {
                response = responder_(request);
            } catch (...) {
            }
            std::size_t sent = 0;
            while (sent < response.size() && running_) {
                const ssize_t n =
                    ::send(client_fd, response.data() + sent,
                           response.size() - sent, 0);
                if (n <= 0) {
                    break;
                }
                sent += static_cast<std::size_t>(n);
            }
            ::close(client_fd);
        }
    }

    int fd_ = -1;
    std::uint16_t port_ = 0;
    std::atomic<bool> running_{true};
    std::mutex wake_mutex_;
    std::condition_variable wake_;
    std::thread thread_;
    std::function<std::string(const std::string&)> responder_;
    std::string last_request_;
};

HttpRequest get_request(const std::string& url) {
    HttpRequest request;
    request.method = "GET";
    request.url = url;
    request.timeout_ms = 2000;
    return request;
}

}  // namespace

TEST(SocketNetwork, DecodesChunkedTransferEncoding) {
    LoopbackServer server([](const std::string&) {
        return "HTTP/1.1 200 OK\r\n"
               "Content-Type: text/plain\r\n"
               "Transfer-Encoding: chunked\r\n\r\n"
               "5\r\nhello\r\n"
               "6\r\n world\r\n"
               "0\r\n\r\n";
    });
    auto client = prowsetk::make_socket_network_client();
    const HttpResponse response = client->send(get_request(server.url("/")));
    EXPECT_EQ(response.status, 200);
    EXPECT_EQ(response.body, "hello world");
}

TEST(SocketNetwork, ReadsContentLengthBodiesExactly) {
    LoopbackServer server([](const std::string&) {
        return "HTTP/1.1 200 OK\r\n"
               "Content-Length: 11\r\n"
               "Content-Type: text/plain\r\n\r\n"
               "hello world";
    });
    auto client = prowsetk::make_socket_network_client();
    const HttpResponse response = client->send(get_request(server.url("/")));
    EXPECT_EQ(response.status, 200);
    EXPECT_EQ(response.body, "hello world");
}

TEST(SocketNetwork, SendsMethodAndBody) {
    LoopbackServer server([](const std::string& request) {
        (void)request;
        return "HTTP/1.1 204 No Content\r\nContent-Length: 0\r\n\r\n";
    });
    auto client = prowsetk::make_socket_network_client();
    HttpRequest request = get_request(server.url("/items"));
    request.method = "POST";
    request.body = "name=widget&qty=3";
    const HttpResponse response = client->send(request);
    EXPECT_EQ(response.status, 204);
    EXPECT_NE(server.last_request().find("POST /items HTTP/1.1"),
              std::string::npos);
    EXPECT_NE(server.last_request().find("Content-Length: 17"),
              std::string::npos);
    EXPECT_NE(server.last_request().find("name=widget&qty=3"),
              std::string::npos);
}

TEST(SocketNetwork, EnforcesBodySizeLimit) {
    LoopbackServer server([](const std::string&) {
        std::string body(4096, 'x');
        return "HTTP/1.1 200 OK\r\nContent-Length: 4096\r\n\r\n" + body;
    });
    auto client = prowsetk::make_socket_network_client();
    HttpRequest request = get_request(server.url("/"));
    request.max_response_bytes = 1024;
    EXPECT_THROW(client->send(request), Error);
}

TEST(SocketNetwork, AppliesRequestTimeout) {
    LoopbackServer server([](const std::string&) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        return std::string();
    });
    auto client = prowsetk::make_socket_network_client();
    HttpRequest request = get_request(server.url("/"));
    request.timeout_ms = 300;
    try {
        client->send(request);
        FAIL() << "expected a timeout";
    } catch (const Error& error) {
        EXPECT_EQ(error.code(), prowsetk::ErrorCode::Timeout);
    }
}

#endif  // __unix__ || __APPLE__