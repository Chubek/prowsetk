#ifndef PROWSETK_WEB_INTERFACE_HPP
#define PROWSETK_WEB_INTERFACE_HPP

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "prowsetk/browser.hpp"

namespace prowsetk {

// A minimal HTTP/1.x request received by the web interface. `path` excludes the
// query string; `query` is the raw (undecoded) query component and may be
// empty. Distinct from `HttpRequest` in network_client.hpp, which models an
// outbound request.
struct WebRequest {
    std::string method = "GET";
    std::string path = "/";
    std::string query;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
};

// A minimal HTTP/1.x response produced by the web interface. Callers fill
// `status`, `headers`, and `body`. Distinct from `HttpResponse` in
// network_client.hpp, which models an inbound response.
struct WebResponse {
    int status = 200;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;

    // Convenience factories used throughout the web interface.
    static WebResponse json(int status, std::string body);
    static WebResponse text(int status, std::string body,
                            std::string content_type = "text/plain; charset=utf-8");
    static WebResponse not_found();
    static WebResponse method_not_allowed();
};

// Configuration for the embeddable web interface.
struct WebInterfaceConfig {
    BrowserConfig browser;
    // Directory holding the static web UI (`index.html`, `app.js`, `style.css`).
    // When empty, static-file routes return 404 and only the JSON API is served.
    std::filesystem::path web_root;
};

// The ProwseTk web interface: a headless JSON REST API and a static web UI
// layered over the C++ core. It exposes navigation, document inspection,
// CSS/XPath selection, JavaScript evaluation, and endpoint extraction through
// HTTP so that any host application or external tool can drive the Flatworm
// engine.
//
// All network access performed by the interface is host-mediated through the
// owned `Browser`'s `NetworkClient`; the interface itself never opens arbitrary
// sockets, and secrets are redacted by the engine's `RedactionPolicy`.
class WebInterface {
public:
    explicit WebInterface(WebInterfaceConfig config = {});
    ~WebInterface();

    WebInterface(const WebInterface&) = delete;
    WebInterface& operator=(const WebInterface&) = delete;

    // Routes one HTTP request to a response. Pure and deterministic: it never
    // opens a socket and never throws; failures are returned as error responses
    // with a JSON body.
    WebResponse handle(const WebRequest& request);

    Browser& browser() noexcept { return *browser_; }
    const Browser& browser() const noexcept { return *browser_; }

    const std::filesystem::path& web_root() const noexcept { return web_root_; }

private:
    struct Impl;
    // `browser_` is declared before `impl_` so it is destroyed after the
    // session map: Session destructors report storage release back to the
    // Browser and must observe it while it is still alive.
    std::unique_ptr<Browser> browser_;
    std::unique_ptr<Impl> impl_;
    std::filesystem::path web_root_;

    // Routes one request. Implemented in web_interface.cpp; the sub-routing and
    // JSON handling live in that translation unit and never surface here.
    WebResponse route(const WebRequest& request);
    WebResponse serve_static(const std::string& name);
};

// A minimal blocking HTTP/1.1 server that adapts a `WebInterface` to a POSIX
// listening socket. Headless and dependency-free; single-threaded, so requests
// are served one at a time in a deterministic order.
class HttpServer {
public:
    HttpServer(WebInterface& interface, std::string host, std::uint16_t port);
    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    // The port actually bound. When constructed with port 0, this is the
    // ephemeral port chosen by the operating system.
    std::uint16_t port() const noexcept;

    // Serves connections until stop() is called. Blocks the calling thread.
    void run();

    // Requests the server to stop after the current connection completes.
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace prowsetk

#endif  // PROWSETK_WEB_INTERFACE_HPP
