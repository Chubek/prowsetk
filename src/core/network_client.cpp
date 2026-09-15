#include "prowsetk/network_client.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <mutex>

#include "prowsetk/error.hpp"
#include "prowsetk/url.hpp"

#if defined(__unix__) || defined(__APPLE__)
#include <netdb.h>
#include <sys/socket.h>
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

class SocketNetworkClient : public NetworkClient {
public:
    HttpResponse send(const HttpRequest& request) override {
        const Url url = parse_url(request.url);
        if (url.scheme != "http") {
            throw Error(ErrorCode::Unsupported,
                        "socket client supports plain HTTP only: " + url.scheme);
        }
        const std::string port = url.port.empty() ? "80" : url.port;

        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* result = nullptr;
        const int rc = ::getaddrinfo(url.host.c_str(), port.c_str(), &hints,
                                     &result);
        if (rc != 0 || result == nullptr) {
            throw Error(ErrorCode::NetworkError,
                        "DNS resolution failed for " + url.host);
        }

        int fd = -1;
        for (addrinfo* candidate = result; candidate != nullptr;
             candidate = candidate->ai_next) {
            fd = ::socket(candidate->ai_family, candidate->ai_socktype,
                          candidate->ai_protocol);
            if (fd < 0) {
                continue;
            }
            if (::connect(fd, candidate->ai_addr, candidate->ai_addrlen) == 0) {
                break;
            }
            ::close(fd);
            fd = -1;
        }
        ::freeaddrinfo(result);
        if (fd < 0) {
            throw Error(ErrorCode::NetworkError,
                        "connection failed for " + url.host + ":" + port);
        }

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
            if (n <= 0) {
                ::close(fd);
                throw Error(ErrorCode::NetworkError, "socket send failed");
            }
            sent += static_cast<std::size_t>(n);
        }

        std::string raw;
        char buffer[8192];
        while (raw.size() < request.max_response_bytes) {
            const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
            if (n <= 0) {
                break;
            }
            raw.append(buffer, static_cast<std::size_t>(n));
        }
        ::close(fd);

        const auto header_end = raw.find("\r\n\r\n");
        if (header_end == std::string::npos) {
            throw Error(ErrorCode::NetworkError, "malformed HTTP response");
        }
        std::string head = raw.substr(0, header_end);
        HttpResponse response;
        response.body = raw.substr(header_end + 4);
        response.final_url = request.url;

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
        return response;
    }

    std::string name() const override { return "socket"; }
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
