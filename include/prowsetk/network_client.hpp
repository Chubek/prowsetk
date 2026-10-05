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

class WebSocket;

enum class ProxyScheme { None, Http, Https, Socks5 };
struct ProxyConfig {
    ProxyScheme scheme = ProxyScheme::None;
    std::string host;
    std::string port;
    std::string username;
    std::string password;
    bool enabled() const noexcept { return scheme != ProxyScheme::None && !host.empty() && !port.empty(); }
};
ProxyConfig parse_proxy_url(std::string_view value);
// Scheme-specific HTTPS_PROXY/HTTP_PROXY (uppercase, then lowercase), falling
// back to ALL_PROXY. NO_PROXY/no_proxy host/suffix/port exclusions apply only
// to environment-selected proxies; explicit request/client proxies override it.
ProxyConfig proxy_from_environment(std::string_view target_url);
std::string proxy_to_string(const ProxyConfig& proxy, bool redact = true);

struct HttpRequest {
    std::string method = "GET";
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    int timeout_ms = 30000;
    std::size_t max_response_bytes = 32u * 1024u * 1024u;
    ProxyConfig proxy;
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

    // Explicit host-mediated RFC 6455 upgrade. request.url uses ws/wss; no
    // redirects, cookies or page headers are implicitly attached.
    virtual std::unique_ptr<WebSocket> open_websocket(const HttpRequest& request);

    virtual std::string name() const { return "network"; }
    virtual void set_proxy(ProxyConfig proxy) { (void)proxy; }
    virtual ProxyConfig proxy() const { return {}; }
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
    void set_proxy(ProxyConfig proxy) override { (void)proxy; }


    const std::vector<HttpRequest>& requests() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// POSIX socket client. With OpenSSL 3 available at build time, HTTPS verifies
// the certificate chain and hostname using OpenSSL's default trust paths.
// HTTP(S) proxies use absolute-form HTTP or CONNECT for HTTPS. Proxy Basic
// credentials never reach a tunneled origin; HTTPS proxies also verify TLS.
// Builds without OpenSSL report HTTPS as unsupported; HTTP remains available.
std::unique_ptr<NetworkClient> make_socket_network_client();

}  // namespace prowsetk

#endif  // PROWSETK_NETWORK_CLIENT_HPP
