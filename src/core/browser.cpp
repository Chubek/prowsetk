#include "prowsetk/browser.hpp"

#include <algorithm>
#include <sstream>

#include "prowsetk/error.hpp"
#include "prowsetk/javascript_runtime.hpp"
#include "prowsetk/lua_runtime.hpp"
#include "prowsetk/url.hpp"

namespace prowsetk {
namespace {

std::string header_value(
    const std::vector<std::pair<std::string, std::string>>& headers,
    std::string_view name) {
    for (const auto& [key, value] : headers) {
        if (key.size() == name.size() &&
            std::equal(key.begin(), key.end(), name.begin(),
                       [](char a, char b) {
                           return std::tolower(static_cast<unsigned char>(a)) ==
                                  std::tolower(static_cast<unsigned char>(b));
                       })) {
            return value;
        }
    }
    return {};
}

}  // namespace

Browser::Browser(BrowserConfig config) : config_(std::move(config)) {
    try {
        network_ = make_socket_network_client();
    } catch (...) {
        network_ = std::make_unique<MemoryNetworkClient>();
    }
    storage_ = std::make_unique<MemoryStorage>();
    plugins_ = std::make_unique<PluginRegistry>();
    wasm_ = make_wasm_runtime();
    lua_ = std::make_unique<LuaRuntime>();
    if (lua_->available()) {
        lua_->bind_browser(this);
    }
}

Browser::~Browser() = default;

std::shared_ptr<Session> Browser::create_session(SessionConfig config) {
    const std::string id = "session-" + std::to_string(++session_counter_);
    auto session = std::shared_ptr<Session>(
        new Session(this, std::move(config), id));
    Event event;
    event.type = EventType::SessionCreated;
    event.name = id;
    events_.emit(event);
    return session;
}

void Browser::set_network_client(std::unique_ptr<NetworkClient> client) {
    if (client != nullptr) {
        network_ = std::move(client);
    }
}

NetworkClient& Browser::network_client() { return *network_; }
const NetworkClient& Browser::network_client() const { return *network_; }

std::unique_ptr<JavaScriptRuntime> Browser::create_javascript_runtime() const {
    return make_null_javascript_runtime();
}

CapabilitySet Browser::capabilities() const {
    CapabilitySet capabilities;
    capabilities.set("engine", ImplementationClass::PartiallyImplemented,
                     "Flatworm automation-oriented engine");
    capabilities.set("html-parser", ImplementationClass::PartiallyImplemented,
                     "tolerant subset parser");
    capabilities.set("css-selectors", ImplementationClass::PartiallyImplemented,
                     "type, class, id, attribute, and structural pseudos");
    capabilities.set("network", ImplementationClass::ImplementedWithRestrictions,
                     "host-mediated; plain HTTP over sockets by default");
    capabilities.set("storage", ImplementationClass::FullyImplemented,
                     "in-memory cookies, local and session storage");
    capabilities.set("events", ImplementationClass::FullyImplemented);
    capabilities.set("endpoint-extraction",
                     ImplementationClass::PartiallyImplemented,
                     "heuristic; provenance and confidence preserved");

    for (const auto& capability : web_platform().capabilities().all()) {
        capabilities.add(capability);
    }
    for (const auto& capability : wasm_->capabilities().all()) {
        capabilities.add(capability);
    }
    auto javascript = create_javascript_runtime();
    for (const auto& capability : javascript->capabilities().all()) {
        capabilities.add(capability);
    }
    capabilities.set("lua",
                     lua_ != nullptr && lua_->available()
                         ? ImplementationClass::FullyImplemented
                         : ImplementationClass::Unsupported,
                     "lprowse automation bindings");
    return capabilities;
}

WebPlatform Browser::web_platform() const { return default_web_platform(); }

Session::Session(Browser* browser, SessionConfig config, std::string id)
    : browser_(browser), config_(std::move(config)), id_(std::move(id)) {
    if (browser_->config().javascript) {
        javascript_ = browser_->create_javascript_runtime();
    }
}

Session::~Session() {
    if (browser_ != nullptr) {
        browser_->storage().release_session(id_);
    }
}

HttpResponse Session::fetch(std::string_view url) {
    std::string current(url);
    int redirects = 0;
    std::vector<std::string> chain;

    while (true) {
        const Url parsed = parse_url(current);
        HttpRequest request;
        request.method = "GET";
        request.url = current;
        request.timeout_ms = browser_->config().timeout_ms;
        request.max_response_bytes = browser_->config().max_response_bytes;
        request.headers = headers_;
        if (header_value(request.headers, "User-Agent").empty()) {
            request.headers.emplace_back("User-Agent",
                                         browser_->config().user_agent);
        }
        if (header_value(request.headers, "Accept").empty()) {
            request.headers.emplace_back(
                "Accept", "text/html,application/xhtml+xml,*/*;q=0.8");
        }
        const std::string cookie =
            browser_->storage().cookies().cookie_header(parsed);
        if (!cookie.empty() &&
            header_value(request.headers, "Cookie").empty()) {
            request.headers.emplace_back("Cookie", cookie);
        }

        Event before;
        before.type = EventType::BeforeRequest;
        before.url = current;
        before.attributes["method"] = request.method;
        browser_->events().emit(before);
        if (before.cancelled) {
            throw Error(ErrorCode::SecurityViolation,
                        "request cancelled by handler: " + current);
        }

        HttpResponse response = browser_->network_client().send(request);

        Event after;
        after.type = EventType::AfterResponse;
        after.url = current;
        after.attributes["status"] = std::to_string(response.status);
        browser_->events().emit(after);

        const bool redirect = response.status == 301 || response.status == 302 ||
                              response.status == 303 ||
                              response.status == 307 ||
                              response.status == 308;
        const std::string location = response.header("Location");
        if (!redirect || location.empty() ||
            !browser_->config().follow_redirects) {
            response.final_url = current;
            response.redirect_chain = chain;
            return response;
        }
        if (redirects >= browser_->config().max_redirects) {
            throw Error(ErrorCode::TooManyRedirects,
                        "exceeded " +
                            std::to_string(browser_->config().max_redirects) +
                            " redirects");
        }
        Event redirect_event;
        redirect_event.type = EventType::BeforeRedirect;
        redirect_event.url = current;
        redirect_event.attributes["location"] = location;
        browser_->events().emit(redirect_event);
        if (redirect_event.cancelled) {
            response.final_url = current;
            response.redirect_chain = chain;
            return response;
        }
        chain.push_back(current);
        current = resolve_url(current, location);
        ++redirects;
    }
}

void Session::navigate(std::string_view url) {
    if (closed_) {
        throw Error(ErrorCode::InvalidArgument, "session is closed");
    }
    Event before;
    before.type = EventType::BeforeNavigation;
    before.url = std::string(url);
    browser_->events().emit(before);
    if (before.cancelled) {
        return;
    }

    const HttpResponse response = fetch(url);
    if (response.status >= 400) {
        throw Error(ErrorCode::NetworkError,
                    "navigation failed with status " +
                        std::to_string(response.status) + " for " +
                        std::string(url));
    }
    install_document(response.body, response.final_url,
                     response.final_url);

    Event after;
    after.type = EventType::AfterNavigation;
    after.url = response.final_url;
    after.attributes["status"] = std::to_string(response.status);
    browser_->events().emit(after);
}

void Session::install_document(std::string_view html, std::string url,
                               std::string base_url) {
    document_ = parse_html(html, std::move(url), std::move(base_url));
    current_url_ = document_->url();

    Event created;
    created.type = EventType::DocumentCreated;
    created.url = current_url_;
    created.name = document_->title();
    browser_->events().emit(created);

    if (javascript_ == nullptr) {
        return;
    }
    if (javascript_->name() == "null") {
        Event unsupported;
        unsupported.type = EventType::UnsupportedApi;
        unsupported.name = "javascript";
        unsupported.message =
            "page scripts were not executed: no JavaScript engine";
        browser_->events().emit(unsupported);
        return;
    }
    for (const auto& script : document_->scripts()) {
        if (script->has_attribute("src")) {
            continue;
        }
        Event begin;
        begin.type = EventType::BeforeScript;
        begin.url = current_url_;
        browser_->events().emit(begin);

        const ScriptResult result = javascript_->evaluate(script->text());
        Event end;
        end.type = EventType::AfterScript;
        end.url = current_url_;
        if (!result.ok) {
            end.type = EventType::ScriptException;
            end.message = result.error;
        }
        browser_->events().emit(end);
    }
}

void Session::load_html(std::string_view html, std::string_view base_url) {
    if (closed_) {
        throw Error(ErrorCode::InvalidArgument, "session is closed");
    }
    const std::string base =
        base_url.empty() ? current_url_ : std::string(base_url);
    install_document(html, base, base);
}

void Session::set_header(std::string name, std::string value) {
    for (auto& [key, existing] : headers_) {
        if (key == name) {
            existing = std::move(value);
            return;
        }
    }
    headers_.emplace_back(std::move(name), std::move(value));
}

void Session::clear_headers() { headers_.clear(); }

std::vector<std::pair<std::string, std::string>> Session::headers() const {
    return headers_;
}

std::string Session::evaluate_js(std::string_view script,
                                 const ScriptOptions& options) {
    if (javascript_ == nullptr) {
        throw Error(ErrorCode::Unsupported,
                    "JavaScript is disabled for this session");
    }
    const ScriptResult result = javascript_->evaluate(script, options);
    if (!result.ok) {
        Event event;
        event.type = EventType::ScriptException;
        event.url = current_url_;
        event.message = result.error;
        browser_->events().emit(event);
        throw Error(ErrorCode::JavaScriptError, result.error);
    }
    return result.value;
}

CookieJar& Session::cookies() noexcept {
    return browser_->storage().cookies();
}

KeyValueStore& Session::local_storage() noexcept {
    return browser_->storage().local_storage(id_);
}

KeyValueStore& Session::session_storage() noexcept {
    return browser_->storage().session_storage(id_);
}

CapabilitySet Session::capabilities() const {
    CapabilitySet capabilities = browser_->capabilities();
    capabilities.set("session-isolation", ImplementationClass::FullyImplemented);
    return capabilities;
}

void Session::close() {
    if (closed_) {
        return;
    }
    closed_ = true;
    browser_->storage().release_session(id_);
    Event event;
    event.type = EventType::SessionDestroyed;
    event.name = id_;
    browser_->events().emit(event);
}

}  // namespace prowsetk
