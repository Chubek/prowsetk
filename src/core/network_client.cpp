#include "prowsetk/network_client.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <mutex>

#include "prowsetk/error.hpp"
#include "prowsetk/url.hpp"

#if defined(__unix__) || defined(__APPLE__)
#include <cerrno>
#include <cstddef>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace prowsetk {
namespace {

std::string to_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    });
    return value;
}

std::string trim(std::string value) {
    const auto not_space = [](unsigned char c) { return std::isspace(c) == 0; };
    value.erase(value.begin(),
                std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(),
                value.end());
    return value;
}

}  // namespace

std::string HttpResponse::header(std::string_view name) const {
    const std::string wanted = to_lower(std::string(name));
    for (const auto& [key, value] : headers) {
        if (to_lower(key) == wanted) {
            return value;
        }
    }
    return {};
}

struct MemoryNetworkClient::Impl {
    std::mutex mutex;
    std::map<std::string, HttpResponse> responses;
    std::function<HttpResponse(const HttpRequest&)> handler;
    std::vector<HttpRequest> requests;
};

MemoryNetworkClient::MemoryNetworkClient() : impl_(std::make_unique<Impl>()) {}
MemoryNetworkClient::~MemoryNetworkClient() = default;

void MemoryNetworkClient::set_response(std::string url, HttpResponse response) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->responses[std::move(url)] = std::move(response);
}

void MemoryNetworkClient::set_handler(
    std::function<HttpResponse(const HttpRequest&)> handler) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->handler = std::move(handler);
}

HttpResponse MemoryNetworkClient::send(const HttpRequest& request) {
    std::function<HttpResponse(const HttpRequest&)> handler;
    HttpResponse response;
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->requests.push_back(request);
        handler = impl_->handler;
        const auto it = impl_->responses.find(request.url);
        if (it != impl_->responses.end()) {
            response = it->second;
            found = true;
        }
    }
    if (handler) {
        response = handler(request);
        found = true;
    }
    if (!found) {
        throw Error(ErrorCode::NotFound,
                    "no response registered for " + request.url);
    }
    if (response.final_url.empty()) {
        response.final_url = request.url;
    }
    return response;
}

const std::vector<HttpRequest>& MemoryNetworkClient::requests() const {
    return impl_->requests;
}

#if defined(__unix__) || defined(__APPLE__)
namespace {

std::string read_status_line_error() {
    return "malformed HTTP response";
}

class SocketNetworkClient : public NetworkClient {
public:
    HttpResponse send(const HttpRequest& request) override {
        const Url url = parse_url(request.url);
        if (url.scheme != "http") {
            throw Error(ErrorCode::Unsupported,
                        "socket client supports plain HTTP only: " + url.scheme);
        }
        const std::string port = url.port.empty() ? "80" : url.port;

        int fd = open_connection(url.host, port, request.timeout_ms);

        std::string path = url.path.empty() ? "/" : url.path;
        if (!url.query.empty()) {
            path += "?" + url.query;
        }
        std::string payload =
            request.method + " " + path + " HTTP/1.1\r\nHost: " + url.host;
        bool has_user_agent = false;
        for (const auto& [name, value] : request.headers) {
            payload += "\r\n" + name + ": " + value;
            if (to_lower(name) == "user-agent") {
                has_user_agent = true;
            }
        }
        if (!has_user_agent) {
            payload += "\r\nUser-Agent: ProwseTk/0.1";
        }
        payload += "\r\nConnection: close\r\n";
        if (!request.body.empty()) {
            payload += "Content-Length: " +
                       std::to_string(request.body.size()) + "\r\n";
        }
        payload += "\r\n" + request.body;

        std::size_t sent = 0;
        while (sent < payload.size()) {
            const ssize_t n = ::send(fd, payload.data() + sent,
                                     payload.size() - sent, 0);
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                ::close(fd);
                throw Error(ErrorCode::Timeout,
                            "request timed out while sending to " + url.host);
            }
            if (n <= 0) {
                ::close(fd);
                throw Error(ErrorCode::NetworkError, "socket send failed");
            }
            sent += static_cast<std::size_t>(n);
        }

        HttpResponse response;
        try {
            response = read_response(fd, request.max_response_bytes);
        } catch (...) {
            ::close(fd);
            throw;
        }
        ::close(fd);
        response.final_url = request.url;
        return response;
    }

    std::string name() const override { return "socket"; }

private:
    int open_connection(const std::string& host, const std::string& port,
                        int timeout_ms) {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* result = nullptr;
        const int rc = ::getaddrinfo(host.c_str(), port.c_str(), &hints,
                                     &result);
        if (rc != 0 || result == nullptr) {
            throw Error(ErrorCode::NetworkError,
                        "DNS resolution failed for " + host);
        }

        int fd = -1;
        int last_error = ECONNREFUSED;
        const bool bounded = timeout_ms > 0;
        for (addrinfo* candidate = result; candidate != nullptr;
             candidate = candidate->ai_next) {
            fd = ::socket(candidate->ai_family, candidate->ai_socktype,
                          candidate->ai_protocol);
            if (fd < 0) {
                continue;
            }
            const int flags = ::fcntl(fd, F_GETFL, 0);
            ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
            if (::connect(fd, candidate->ai_addr, candidate->ai_addrlen) == 0) {
                ::fcntl(fd, F_SETFL, flags);
                break;
            }
            if (errno == EINPROGRESS && bounded) {
                pollfd poll_fd{fd, POLLOUT, 0};
                const int poll_rc =
                    ::poll(&poll_fd, 1, timeout_ms);
                if (poll_rc > 0 && (poll_fd.revents & POLLOUT)) {
                    int socket_error = 0;
                    socklen_t error_len = sizeof(socket_error);
                    ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error,
                                 &error_len);
                    if (socket_error == 0) {
                        ::fcntl(fd, F_SETFL, flags);
                        break;
                    }
                    last_error = socket_error;
                } else if (poll_rc == 0) {
                    last_error = ETIMEDOUT;
                }
            } else {
                last_error = errno;
            }
            ::close(fd);
            fd = -1;
        }
        ::freeaddrinfo(result);
        if (fd < 0) {
            if (last_error == ETIMEDOUT) {
                throw Error(ErrorCode::Timeout,
                            "connection to " + host + ":" + port +
                                " timed out");
            }
            throw Error(ErrorCode::NetworkError,
                        "connection failed for " + host + ":" + port);
        }

        if (bounded) {
            const timeval timeout{timeout_ms / 1000,
                                  (timeout_ms % 1000) * 1000};
            ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                         sizeof(timeout));
            ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                         sizeof(timeout));
        }
        return fd;
    }

    static bool recv_more(int fd, std::string& raw) {
        char buffer[8192];
        const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            throw Error(ErrorCode::Timeout,
                        "response read timed out before completion");
        }
        if (n <= 0) {
            return false;
        }
        raw.append(buffer, static_cast<std::size_t>(n));
        return true;
    }

    // Reads until `needle` appears at or after `offset` in `raw`, appending
    // socket data as required. Returns false on EOF before a match.
    static bool fill_until(int fd, std::string& raw, std::size_t offset,
                           const std::string& needle) {
        while (true) {
            const std::size_t found = raw.find(needle, offset);
            if (found != std::string::npos) {
                return true;
            }
            if (!recv_more(fd, raw)) {
                return false;
            }
        }
    }

    // Decodes a chunked-transfer body beginning at `raw[offset]`. Appends to
    // `raw` as needed and returns the decoded body.
    static std::string decode_chunked(int fd, std::string& raw,
                                      std::size_t offset,
                                      std::size_t max_bytes) {
        std::string body;
        while (true) {
            if (!fill_until(fd, raw, offset, "\r\n")) {
                throw Error(ErrorCode::NetworkError,
                            "truncated chunked response");
            }
            const std::size_t line_end = raw.find("\r\n", offset);
            std::string line = raw.substr(offset, line_end - offset);
            offset = line_end + 2;

            const auto semicolon = line.find(';');
            if (semicolon != std::string::npos) {
                line.resize(semicolon);
            }
            const auto trim_hex = [](std::string value) {
                const auto not_space = [](unsigned char c) {
                    return std::isspace(c) == 0;
                };
                value.erase(value.begin(),
                            std::find_if(value.begin(), value.end(),
                                         not_space));
                value.erase(std::find_if(value.rbegin(), value.rend(),
                                         not_space)
                                .base(),
                            value.end());
                return value;
            };
            line = trim_hex(std::move(line));
            if (line.empty()) {
                throw Error(ErrorCode::NetworkError,
                            "invalid chunked size line");
            }
            std::size_t chunk_size = 0;
            const char* begin = line.c_str();
            char* end = nullptr;
            const unsigned long parsed =
                std::strtoul(begin, &end, 16);
            if (end == begin) {
                throw Error(ErrorCode::NetworkError,
                            "invalid chunked size");
            }
            chunk_size = static_cast<std::size_t>(parsed);

            if (chunk_size == 0) {
                if (!fill_until(fd, raw, offset, "\r\n")) {
                    throw Error(ErrorCode::NetworkError,
                                "truncated chunked trailer");
                }
                return body;
            }
            if (body.size() + chunk_size > max_bytes) {
                throw Error(ErrorCode::ResourceLimit,
                            "response body exceeds the configured limit");
            }
            if (!fill_until(fd, raw, offset + chunk_size, "\r\n")) {
                throw Error(ErrorCode::NetworkError,
                            "truncated chunk data");
            }
            body.append(raw, offset, chunk_size);
            offset += chunk_size + 2;
        }
    }

    static HttpResponse read_response(int fd, std::size_t max_bytes) {
        std::string raw;
        if (!fill_until(fd, raw, 0, "\r\n\r\n")) {
            throw Error(ErrorCode::NetworkError, read_status_line_error());
        }
        const std::size_t header_end = raw.find("\r\n\r\n");
        std::string head = raw.substr(0, header_end);

        HttpResponse response;
        const auto line_end = head.find("\r\n");
        const std::string status_line = head.substr(0, line_end);
        const auto first_space = status_line.find(' ');
        if (first_space != std::string::npos) {
            response.status = std::atoi(status_line.c_str() + first_space + 1);
        }
        std::size_t cursor =
            line_end == std::string::npos ? head.size() : line_end + 2;
        while (cursor < head.size()) {
            const auto end = head.find("\r\n", cursor);
            const std::string line =
                head.substr(cursor, end == std::string::npos
                                        ? std::string::npos
                                        : end - cursor);
            const auto colon = line.find(':');
            if (colon != std::string::npos) {
                response.headers.emplace_back(
                    trim(line.substr(0, colon)),
                    trim(line.substr(colon + 1)));
            }
            if (end == std::string::npos) {
                break;
            }
            cursor = end + 2;
        }

        std::size_t body_offset = header_end + 4;
        const std::string transfer_encoding =
            to_lower(response.header("Transfer-Encoding"));
        if (transfer_encoding.find("chunked") != std::string::npos) {
            response.body =
                decode_chunked(fd, raw, body_offset, max_bytes);
            return response;
        }

        const std::string content_length = response.header("Content-Length");
        if (!content_length.empty()) {
            const unsigned long length =
                std::strtoul(content_length.c_str(), nullptr, 10);
            if (length > max_bytes) {
                throw Error(ErrorCode::ResourceLimit,
                            "response body exceeds the configured limit");
            }
            while (raw.size() < body_offset + length) {
                if (!recv_more(fd, raw)) {
                    break;
                }
            }
            response.body = raw.substr(body_offset, length);
            return response;
        }

        while (raw.size() - body_offset < max_bytes) {
            if (!recv_more(fd, raw)) {
                break;
            }
        }
        if (raw.size() - body_offset > max_bytes) {
            throw Error(ErrorCode::ResourceLimit,
                        "response body exceeds the configured limit");
        }
        response.body = raw.substr(body_offset);
        return response;
    }
};

}  // namespace
#endif

std::unique_ptr<NetworkClient> make_socket_network_client() {
#if defined(__unix__) || defined(__APPLE__)
    return std::make_unique<SocketNetworkClient>();
#else
    throw Error(ErrorCode::Unsupported,
                "no socket network client on this platform");
#endif
}

}  // namespace prowsetk
