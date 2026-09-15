#ifndef PROWSETK_BROWSER_HPP
#define PROWSETK_BROWSER_HPP

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "prowsetk/capability.hpp"
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
class LuaRuntime;

struct BrowserConfig {
    std::string user_agent = "ProwseTk/0.1";
    bool javascript = false;
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

    void set_header(std::string name, std::string value);
    void clear_headers();
    std::vector<std::pair<std::string, std::string>> headers() const;

    std::string evaluate_js(std::string_view script,
                            const ScriptOptions& options = {});

    CookieJar& cookies() noexcept;
    KeyValueStore& local_storage() noexcept;
    KeyValueStore& session_storage() noexcept;

    CapabilitySet capabilities() const;
    EventDispatcher& events() noexcept { return browser_->events(); }

    void close();

private:
    friend class Browser;
    Session(Browser* browser, SessionConfig config, std::string id);

    HttpResponse fetch(std::string_view url);
    void install_document(std::string_view html, std::string url,
                          std::string base_url);

    Browser* browser_;
    SessionConfig config_;
    std::string id_;
    std::string current_url_;
    std::vector<std::pair<std::string, std::string>> headers_;
    std::shared_ptr<Document> document_;
    std::unique_ptr<JavaScriptRuntime> javascript_;
    bool closed_ = false;
};

}  // namespace prowsetk

#endif  // PROWSETK_BROWSER_HPP
