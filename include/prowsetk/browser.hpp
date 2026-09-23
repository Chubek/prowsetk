#ifndef PROWSETK_BROWSER_HPP
#define PROWSETK_BROWSER_HPP

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "prowsetk/capability.hpp"
#include "prowsetk/anti_bot_detection.hpp"
#include "prowsetk/document.hpp"
#include "prowsetk/event.hpp"
#include "prowsetk/javascript_runtime.hpp"
#include "prowsetk/network_client.hpp"
#include "prowsetk/plugin_registry.hpp"
#include "prowsetk/redaction.hpp"
#include "prowsetk/storage.hpp"
#include "prowsetk/wasm_runtime.hpp"
#include "prowsetk/web_platform.hpp"

namespace prowsetk {

class Session;
class JavaScriptRuntime;
class FlatwormScriptHost;
class LuaRuntime;

struct BrowserConfig {
    std::string user_agent =
        "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/128.0.0.0 Safari/537.36";
    bool javascript = true;
    bool follow_redirects = true;
    int max_redirects = 10;
    int timeout_ms = 30000;
    std::size_t max_response_bytes = 32u * 1024u * 1024u;
    std::string unsupported_api_behavior = "warn";
    bool observe_network = false;
    RedactionPolicy redaction = RedactionPolicy::defaults();
};

struct SessionConfig {
    std::string profile = "default";
    bool isolate_storage = true;
    bool persist_session = false;
    bool reuse_cookies = false;
};

// Owns global configuration and creates isolated browsing sessions.
class Browser {
public:
    explicit Browser(BrowserConfig config = {});
    ~Browser();

    Browser(const Browser&) = delete;
    Browser& operator=(const Browser&) = delete;

    std::shared_ptr<Session> create_session(SessionConfig config = {});

    void set_network_client(std::unique_ptr<NetworkClient> client);
    NetworkClient& network_client();
    const NetworkClient& network_client() const;

    Storage& storage() noexcept { return *storage_; }
    EventDispatcher& events() noexcept { return events_; }
    PluginRegistry& plugins() noexcept { return *plugins_; }
    WasmRuntime& wasm() noexcept { return *wasm_; }

    LuaRuntime* lua() noexcept { return lua_.get(); }

    std::unique_ptr<JavaScriptRuntime> create_javascript_runtime() const;

    CapabilitySet capabilities() const;
    WebPlatform web_platform() const;

    // Reports an attempted use of an unsupported API according to
    // `config_.unsupported_api_behavior` ("warn" emits an UnsupportedApi
    // event; "exception"/"abort" throw Error(Unsupported); "default"/"dummy"
    // silently continue).
    void handle_unsupported_api(std::string_view name,
                                std::string_view message) const;

    const BrowserConfig& config() const noexcept { return config_; }

private:
    friend class Session;
    BrowserConfig config_;
    EventDispatcher events_;
    std::unique_ptr<NetworkClient> network_;
    std::unique_ptr<Storage> storage_;
    std::unique_ptr<PluginRegistry> plugins_;
    std::unique_ptr<WasmRuntime> wasm_;
    std::unique_ptr<LuaRuntime> lua_;
    std::size_t session_counter_ = 0;
};

// One host-mediated request issued by page JavaScript (`fetch`,
// `XMLHttpRequest`, `sendBeacon`, or a dynamic script or image load).
// Document navigations are not included. Endpoint extraction records these
// when `observe_network` is set.
struct PageScriptRequest {
    std::string method;
    std::string url;
    int status = 0;
    std::string content_type;
};

// Text of an external script fetched host-mediated during document install
// (static `<script src>` in `Session::install_document`, or a dynamic load
// through the page host). Endpoint extraction scans these with
// `EndpointExtractor::observe_script` when `inspect_scripts` is set, so
// `fetch`/`XMLHttpRequest` POSTs defined in bundles surface even when the
// page never calls them during load.
struct PageScriptText {
    std::string url;
    std::string body;
};

// Represents an isolated browsing context: URL, cookies, storage, headers, and
// the currently loaded document.
class Session {
public:
    ~Session();

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    const std::string& id() const noexcept { return id_; }
    Browser& browser() noexcept { return *browser_; }

    // Fetches `url`, follows redirects according to the browser configuration,
    // parses the response body, and installs it as the current document.
    void navigate(std::string_view url);

    // Installs an already-available HTML document. `base_url` resolves relative
    // references; when empty the session's current URL is used.
    void load_html(std::string_view html, std::string_view base_url = {});

    std::shared_ptr<Document> document() const noexcept { return document_; }
    const std::string& current_url() const noexcept { return current_url_; }
    const std::optional<AntiBotDetection>& anti_bot_detection() const noexcept {
        return anti_bot_detection_;
    }

    void set_header(std::string name, std::string value);
    void clear_headers();
    std::vector<std::pair<std::string, std::string>> headers() const;

    std::string evaluate_js(std::string_view script,
                            const ScriptOptions& options = {});

    // Requests issued by the current document's scripts since it was
    // installed. Cleared when the next document is installed, except that
    // script-initiated navigations preserve pre-navigation observations
    // (see `follow_script_navigations`).
    const std::vector<PageScriptRequest>& page_script_requests() const noexcept {
        return page_script_requests_;
    }

    // Bodies of external scripts fetched for the current document.
    // Cleared on the same schedule as `page_script_requests_`.
    const std::vector<PageScriptText>& page_script_texts() const noexcept {
        return page_script_texts_;
    }

    // Sends an HTTP request through the browser's NetworkClient, applying the
    // session's default headers and cookies, and follows redirects according to
    // the browser configuration. 301/302/303 convert POST to GET; 307/308
    // preserve the method and body. The response body is not parsed.
    HttpResponse request(HttpRequest request);

    CookieJar& cookies() noexcept;
    KeyValueStore& local_storage() noexcept;
    KeyValueStore& session_storage() noexcept;

    CapabilitySet capabilities() const;
    EventDispatcher& events() noexcept { return browser_->events(); }

    void close();

private:
    friend class Browser;
    Session(Browser* browser, SessionConfig config, std::string id);

    // Builds a default GET request for `url` carrying the session headers,
    // cookie header, and configured timeouts/limits.
    HttpRequest build_request(std::string_view url);

    void install_document(std::string_view html, std::string url,
                          std::string base_url);
    void report_anti_bot_detection(const AntiBotDetection& detection);

    // Emits `event` through the browser dispatcher, stamped with this
    // session's id so Lua `session:on` subscriptions stay scoped.
    void emit_event(Event event);
    std::string document_element_class_name() const;
    void set_document_element_class_name(std::string_view value);

    // Runs the page lifecycle (DOMContentLoaded/load, timers, async script
    // callbacks) to a bounded quiescence after the document's scripts.
    void run_script_lifecycle();

    // Performs script-initiated navigations (location, link clicks, form
    // submits) recorded by the document host after a navigation completes.
    void follow_script_navigations();

    std::unique_ptr<FlatwormScriptHost> script_host_;
    Browser* browser_;
    SessionConfig config_;
    std::string id_;
    std::string current_url_;
    std::vector<std::pair<std::string, std::string>> headers_;
    std::shared_ptr<Document> document_;
    std::unique_ptr<JavaScriptRuntime> javascript_;
    std::optional<AntiBotDetection> anti_bot_detection_;
    std::vector<PageScriptRequest> page_script_requests_;
    std::vector<PageScriptText> page_script_texts_;
    bool closed_ = false;
};

}  // namespace prowsetk

#endif  // PROWSETK_BROWSER_HPP
