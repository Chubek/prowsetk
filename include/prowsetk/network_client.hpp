#ifndef PROWSETK_NETWORK_CLIENT_HPP
#define PROWSETK_NETWORK_CLIENT_HPP

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace prowsetk {

struct HttpRequest {
    std::string method = "GET";
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    int timeout_ms = 30000;
    std::size_t max_response_bytes = 32u * 1024u * 1024u;
};

struct HttpResponse {
    int status = 0;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    std::string final_url;
    std::vector<std::string> redirect_chain;

    bool ok() const noexcept { return status >= 200 && status < 300; }
    std::string header(std::string_view name) const;
};

// Performs HTTP/HTTPS requests. Network access is always host-mediated; plugins
// and WASM modules never open sockets themselves (README "WASI policy").
class NetworkClient {
public:
    virtual ~NetworkClient() = default;

    virtual HttpResponse send(const HttpRequest& request) = 0;

    virtual std::string name() const { return "network"; }
};

// Deterministic, hermetic client for tests and offline automation. Responses
// are pre-registered per URL or supplied by a handler.
class MemoryNetworkClient : public NetworkClient {
public:
    MemoryNetworkClient();
    ~MemoryNetworkClient() override;

    void set_response(std::string url, HttpResponse response);
    void set_handler(std::function<HttpResponse(const HttpRequest&)> handler);

    HttpResponse send(const HttpRequest& request) override;
    std::string name() const override { return "memory"; }

    const std::vector<HttpRequest>& requests() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Plain-HTTP client built on POSIX sockets. HTTPS requires a TLS provider and
// is reported as unsupported until one is wired in.
std::unique_ptr<NetworkClient> make_socket_network_client();

}  // namespace prowsetk

#endif  // PROWSETK_NETWORK_CLIENT_HPP
