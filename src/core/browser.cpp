#include "prowsetk/browser.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <ctime>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>

#include "prowsetk/error.hpp"
#include "prowsetk/flatworm_host.hpp"
#include "prowsetk/javascript_runtime.hpp"
#include "prowsetk/lua_runtime.hpp"
#include "prowsetk/url.hpp"

#include "flatworm/dom_internal.hpp"

namespace prowsetk {
namespace {

bool header_name_equal(std::string_view left, std::string_view right) {
    return left.size() == right.size() &&
           std::equal(left.begin(), left.end(), right.begin(),
                      [](char a, char b) {
                          return std::tolower(static_cast<unsigned char>(a)) ==
                                 std::tolower(static_cast<unsigned char>(b));
                      });
}

bool sensitive_redirect_header(std::string_view name) {
    return header_name_equal(name, "Authorization") ||
           header_name_equal(name, "Proxy-Authorization") ||
           header_name_equal(name, "Cookie");
}

bool same_origin(const Url& left, const Url& right) {
    return left.scheme == right.scheme && left.host == right.host &&
           left.port == right.port && left.has_authority && right.has_authority;
}

std::string header_value(
    const std::vector<std::pair<std::string, std::string>>& headers,
    std::string_view name) {
    for (const auto& [key, value] : headers) {
        if (header_name_equal(key, name)) {
            return value;
        }
    }
    return {};
}

std::string trim(std::string value) {
    const auto not_space = [](unsigned char c) {
        return std::isspace(c) == 0;
    };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(),
                                            not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(),
                value.end());
    return value;
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    });
    return value;
}

std::int64_t utc_time(std::tm value) {
#if defined(_WIN32)
    return static_cast<std::int64_t>(_mkgmtime(&value));
#else
    return static_cast<std::int64_t>(::timegm(&value));
#endif
}

std::optional<std::int64_t> parse_cookie_date(const std::string& value) {
    const char* formats[] = {"%a, %d %b %Y %H:%M:%S GMT",
                             "%A, %d-%b-%y %H:%M:%S GMT",
                             "%a %b %d %H:%M:%S %Y"};
    for (const char* format : formats) {
        std::tm parsed{};
        std::istringstream stream(value);
        stream >> std::get_time(&parsed, format);
        if (!stream.fail()) {
            return utc_time(parsed);
        }
    }
    return std::nullopt;
}

std::int64_t add_seconds(std::int64_t timestamp, std::int64_t seconds) {
    const auto maximum = std::numeric_limits<std::int64_t>::max();
    const auto minimum = std::numeric_limits<std::int64_t>::min();
    if (seconds > 0 && seconds > maximum - timestamp) {
        return maximum;
    }
    if (seconds < 0 && seconds < minimum + timestamp) {
        return minimum;
    }
    return timestamp + seconds;
}

std::string default_cookie_path(const Url& origin) {
    const std::string path = origin.path.empty() ? "/" : origin.path;
    if (path.front() != '/') {
        return "/";
    }
    const auto slash = path.rfind('/');
    if (slash == 0) {
        return "/";
    }
    return path.substr(0, slash);
}

std::optional<Cookie> parse_set_cookie(std::string_view header,
                                       const Url& origin) {
    const auto first_separator = header.find(';');
    const std::string first = trim(std::string(header.substr(
        0, first_separator == std::string_view::npos ? header.size()
                                                       : first_separator)));
    const auto equals = first.find('=');
    if (equals == std::string::npos || equals == 0) {
        return std::nullopt;
    }

    Cookie cookie;
    cookie.name = trim(first.substr(0, equals));
    cookie.value = trim(first.substr(equals + 1));
    cookie.path = default_cookie_path(origin);
    if (cookie.name.empty()) {
        return std::nullopt;
    }

    std::optional<std::int64_t> max_age;
    std::optional<std::int64_t> expires_date;
    std::size_t start = first_separator == std::string_view::npos
                            ? header.size()
                            : first_separator + 1;
    while (start < header.size()) {
        const auto end = header.find(';', start);
        const std::string attribute = trim(std::string(header.substr(
            start, end == std::string_view::npos ? header.size() - start
                                                   : end - start)));
        const auto attribute_equals = attribute.find('=');
        const std::string name = lower(trim(attribute.substr(0, attribute_equals)));
        const std::string value =
            attribute_equals == std::string::npos
                ? std::string()
                : trim(attribute.substr(attribute_equals + 1));
        if (name == "domain" && !value.empty()) {
            cookie.domain = value;
            cookie.host_only = false;
        } else if (name == "path" && !value.empty() && value.front() == '/') {
            cookie.path = value;
        } else if (name == "secure") {
            cookie.secure = true;
        } else if (name == "httponly") {
            cookie.http_only = true;
        } else if (name == "samesite") {
            cookie.same_site = value;
        } else if (name == "max-age") {
            std::int64_t seconds = 0;
            const auto* begin = value.data();
            const auto* end_value = begin + value.size();
            const auto parsed = std::from_chars(begin, end_value, seconds);
            if (parsed.ec == std::errc() && parsed.ptr == end_value) {
                max_age = seconds;
            }
        } else if (name == "expires") {
            if (const auto parsed = parse_cookie_date(value)) {
                expires_date = parsed;
            }
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }

    if (max_age.has_value()) {
        const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
        cookie.expires_unix = add_seconds(now, *max_age);
    } else if (expires_date.has_value()) {
        cookie.expires_unix = expires_date;
    }

    if (cookie.secure && origin.scheme != "https") {
        return std::nullopt;
    }
    if (!cookie.domain.empty()) {
        while (!cookie.domain.empty() && cookie.domain.front() == '.') {
            cookie.domain.erase(cookie.domain.begin());
        }
        cookie.domain = lower(cookie.domain);
        if (cookie.domain != origin.host &&
            (origin.host.size() <= cookie.domain.size() ||
             origin.host.compare(origin.host.size() - cookie.domain.size(),
                                 cookie.domain.size(), cookie.domain) != 0 ||
             origin.host[origin.host.size() - cookie.domain.size() - 1] != '.')) {
            return std::nullopt;
        }
    }
    return cookie;
}

void apply_response_cookies(Browser& browser, const Url& origin,
                            const HttpResponse& response) {
    for (const auto& [name, value] : response.headers) {
        if (!header_name_equal(name, "Set-Cookie")) {
            continue;
        }
        const auto cookie = parse_set_cookie(value, origin);
        if (!cookie) {
            continue;
        }
        browser.storage().cookies().set(origin, *cookie);
        Event event;
        event.type = EventType::CookieChange;
        event.url = origin.to_string();
        event.name = cookie->name;
        event.attributes["domain"] = cookie->domain.empty() ? origin.host
                                                               : cookie->domain;
        event.attributes["path"] = cookie->path;
        browser.events().emit(event);
    }
}

}  // namespace

Browser::Browser(BrowserConfig config) : config_(std::move(config)) {
    try {
        network_ = make_socket_network_client();
    } catch (...) {
        network_ = std::make_unique<MemoryNetworkClient>();
    }
    storage_ = std::make_unique<MemoryStorage>();
    storage_->set_access_listener([this](std::string_view store,
                                         std::string_view key,
                                         std::string_view operation) {
        Event event;
        event.type = EventType::StorageAccess;
        event.name = std::string(operation);
        event.attributes["store"] = std::string(store);
        event.attributes["key"] = std::string(key);
        events_.emit(event);
    });
    plugins_ = std::make_unique<PluginRegistry>();
    plugins_->set_event_dispatcher(&events_);
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
    return make_javascript_runtime();
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

void Browser::handle_unsupported_api(std::string_view name,
                                     std::string_view message) const {
    const std::string behavior = config_.unsupported_api_behavior;
    if (behavior == "exception" || behavior == "abort") {
        throw Error(ErrorCode::Unsupported, std::string(message));
    }
    if (behavior == "default" || behavior == "dummy") {
        return;
    }
    Event event;
    event.type = EventType::UnsupportedApi;
    event.name = std::string(name);
    event.message = std::string(message);
    events_.emit(event);
}

Session::Session(Browser* browser, SessionConfig config, std::string id)
    : browser_(browser), config_(std::move(config)), id_(std::move(id)) {
    script_host_ = std::make_unique<FlatwormScriptHost>();
    script_host_->set_request_hook([this](const HostRequest& host_request) {
        HostResponse out;
        try {
            HttpRequest request;
            request.method = host_request.method;
            request.url = host_request.url;
            request.headers = host_request.headers;
            request.body = host_request.body;
            const HttpResponse response =
                this->request(std::move(request));
            out.status = response.status;
            out.headers = response.headers;
            out.body = response.body;
            out.final_url = response.final_url;
            out.ok = true;
            PageScriptRequest observed;
            observed.method = host_request.method;
            // Record the URL the script addressed, not the redirect target:
            // a 303 after a POST would otherwise attribute POST to a GET page.
            observed.url = host_request.url;
            observed.status = response.status;
            observed.content_type = response.header("Content-Type");
            page_script_requests_.push_back(std::move(observed));
            // Retain dynamic script bodies (e.g. `<script src>` injected or
            // loaded by page script) for static endpoint scanning. Only
            // successful GETs with a script-like target or content type are
            // kept, bounded to avoid unbounded memory growth.
            if (host_request.method == "GET" && response.status >= 200 &&
                response.status < 300 && response.body.size() <= 2u * 1024u * 1024u) {
                const std::string content_type =
                    response.header("Content-Type");
                std::string lowered;
                lowered.reserve(content_type.size());
                for (char c : content_type) {
                    lowered.push_back(static_cast<char>(std::tolower(
                        static_cast<unsigned char>(c))));
                }
                const bool script_type =
                    lowered.find("javascript") != std::string::npos ||
                    lowered.find("ecmascript") != std::string::npos ||
                    lowered.empty();
                std::string path = host_request.url;
                try {
                    path = parse_url(host_request.url).path;
                } catch (...) {
                }
                const bool script_path =
                    path.size() >= 3 &&
                    path.compare(path.size() - 3, 3, ".js") == 0;
                if (script_type || script_path) {
                    page_script_texts_.push_back(
                        PageScriptText{host_request.url, response.body});
                }
            }
        } catch (const std::exception& error) {
            out.error = error.what();
        }
        return out;
    });
    script_host_->set_cookie_hooks(
        [this]() -> std::string {
            if (document_ == nullptr) return {};
            try {
                return browser_->storage().cookies().cookie_header(
                    parse_url(document_->url()));
            } catch (const std::exception&) {
                return {};
            }
        },
        [this](std::string_view cookie) {
            if (document_ == nullptr) return;
            try {
                const Url origin = parse_url(document_->url());
                const auto parsed = parse_set_cookie(cookie, origin);
                if (!parsed.has_value()) return;
                browser_->storage().cookies().set(origin, *parsed);
                Event event;
                event.type = EventType::CookieChange;
                event.url = origin.to_string();
                event.name = parsed->name;
                emit_event(event);
            } catch (const std::exception&) {
                // A malformed document.cookie write is ignored, like in browsers.
            }
        });
    script_host_->set_storage_hook(
        [this](std::string_view area) -> KeyValueStore* {
            return area == "session" ? &session_storage() : &local_storage();
        });
    script_host_->set_navigator_hook([this]() {
        NavigatorInfo info;
        info.user_agent = browser_->config().user_agent;
        info.platform = "Linux x86_64";
        info.language = "en-US";
        return info;
    });
    if (browser_->config().javascript) {
        javascript_ = browser_->create_javascript_runtime();
        javascript_->set_document_host(script_host_.get());
        javascript_->set_console_handler([this](const ConsoleMessage& message) {
            Event event;
            event.type = EventType::Console;
            event.url = current_url_;
            event.name = message.level;
            event.message = message.text;
            emit_event(event);
        });
    }
}

Session::~Session() {
    if (browser_ != nullptr) {
        browser_->storage().release_session(id_);
    }
}

void Session::emit_event(Event event) {
    event.session_id = id_;
    browser_->events().emit(event);
}

void Session::report_anti_bot_detection(const AntiBotDetection& detection) {
    if (!detection.activated) {
        return;
    }
    anti_bot_detection_ = detection;
    Event event;
    event.type = EventType::AntiBotDetected;
    event.url = detection.url.empty() ? current_url_ : detection.url;
    event.name = detection.category;
    event.message =
        "anti-bot challenge detected heuristically; inspect signals";
    event.attributes["confidence"] = std::to_string(detection.confidence);
    event.attributes["signal-count"] =
        std::to_string(detection.signals.size());
    event.attributes["status"] = std::to_string(detection.status);
    for (std::size_t i = 0; i < detection.signals.size(); ++i) {
        const auto& signal = detection.signals[i];
        const std::string prefix = "signal." + std::to_string(i) + ".";
        event.attributes[prefix + "source"] = signal.source;
        event.attributes[prefix + "name"] = signal.name;
        event.attributes[prefix + "confidence"] =
            std::to_string(signal.confidence);
    }
    event.payload = detection;
    emit_event(event);
}

std::string Session::document_element_class_name() const {
    return script_host_ != nullptr ? script_host_->document_element_class_name()
                                   : std::string{};
}

void Session::set_document_element_class_name(std::string_view value) {
    if (script_host_ != nullptr) {
        script_host_->set_document_element_class_name(value);
    }
}

void Session::run_script_lifecycle() {
    if (javascript_ == nullptr || script_host_ == nullptr) {
        return;
    }
    // Drains DOMContentLoaded/load listeners, due timers, and async script
    // callbacks in bounded passes so a page never hangs document install.
    for (int pass = 0; pass < 16; ++pass) {
        const ScriptResult flushed =
            javascript_->evaluate("__prowsetkFlush();");
        if (!flushed.ok || flushed.value == "0") {
            break;
        }
    }
}

void Session::follow_script_navigations() {
    if (script_host_ == nullptr) {
        return;
    }
    PendingNavigation navigation;
    for (int hop = 0;
         hop < browser_->config().max_redirects &&
         script_host_->consume_pending_navigation(navigation);
         ++hop) {
        if (navigation.url.empty()) {
            continue;
        }
        // Preserve pre-navigation script observations: install_document
        // clears them, but a POST issued before a form/link navigation is
        // still backend evidence for endpoint discovery.
        const auto prior_requests = page_script_requests_;
        const auto prior_texts = page_script_texts_;
        try {
            HttpRequest request;
            request.method = navigation.method;
            request.url = navigation.url;
            request.body = navigation.body;
            if (navigation.method == "POST" &&
                header_value(request.headers, "Content-Type").empty()) {
                request.headers.emplace_back("Content-Type",
                                             "application/x-www-form-urlencoded");
            }
            const HttpResponse response = this->request(std::move(request));
            if (response.status >= 400) {
                page_script_requests_ = prior_requests;
                page_script_texts_ = prior_texts;
                break;
            }
            install_document(response.body, response.final_url,
                             response.final_url);
            // Merge prior observations back, deduplicated, then record the
            // navigation itself (a POST navigation is endpoint evidence).
            for (const auto& prior : prior_requests) {
                const auto duplicate = std::find_if(
                    page_script_requests_.begin(), page_script_requests_.end(),
                    [&](const PageScriptRequest& current) {
                        return current.method == prior.method &&
                               current.url == prior.url;
                    });
                if (duplicate == page_script_requests_.end()) {
                    page_script_requests_.push_back(prior);
                }
            }
            for (const auto& prior : prior_texts) {
                const auto duplicate = std::find_if(
                    page_script_texts_.begin(), page_script_texts_.end(),
                    [&](const PageScriptText& current) {
                        return current.url == prior.url;
                    });
                if (duplicate == page_script_texts_.end()) {
                    page_script_texts_.push_back(prior);
                }
            }
            PageScriptRequest nav;
            nav.method = navigation.method.empty() ? "GET" : navigation.method;
            nav.url = navigation.url;
            nav.status = response.status;
            nav.content_type = response.header("Content-Type");
            page_script_requests_.push_back(std::move(nav));
        } catch (const std::exception&) {
            page_script_requests_ = prior_requests;
            page_script_texts_ = prior_texts;
            break;
        }
    }
}

HttpRequest Session::build_request(std::string_view url) {
    HttpRequest request;
    request.method = "GET";
    request.url = std::string(url);
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
    return request;
}

HttpResponse Session::request(HttpRequest request) {
    if (closed_) {
        throw Error(ErrorCode::InvalidArgument, "session is closed");
    }
    int redirects = 0;
    std::vector<std::string> chain;

    // Public request() calls are session requests too. Preserve explicit
    // per-request headers while supplying session defaults for missing names.
    for (const auto& [name, value] : headers_) {
        if (header_value(request.headers, name).empty()) {
            request.headers.emplace_back(name, value);
        }
    }

    while (true) {
        if (header_value(request.headers, "User-Agent").empty()) {
            request.headers.emplace_back("User-Agent",
                                         browser_->config().user_agent);
        }
        if (header_value(request.headers, "Accept").empty()) {
            request.headers.emplace_back(
                "Accept", "text/html,application/xhtml+xml,*/*;q=0.8");
        }

        Event before;
        before.type = EventType::BeforeRequest;
        before.url = request.url;
        before.attributes["method"] = request.method;
        emit_event(before);
        if (before.cancelled) {
            throw Error(ErrorCode::SecurityViolation,
                        "request cancelled by handler: " + request.url);
        }

        browser_->plugins().dispatch_before_request(request);

        const Url parsed = parse_url(request.url);
        const std::string cookie =
            browser_->storage().cookies().cookie_header(parsed);
        if (!cookie.empty() &&
            header_value(request.headers, "Cookie").empty()) {
            request.headers.emplace_back("Cookie", cookie);
        }

        HttpResponse response = browser_->network_client().send(request);
        apply_response_cookies(*browser_, parsed, response);

        Event after;
        after.type = EventType::AfterResponse;
        after.url = request.url;
        after.attributes["method"] = request.method;
        after.attributes["status"] = std::to_string(response.status);
        const std::string content_type = response.header("Content-Type");
        if (!content_type.empty()) {
            after.attributes["content-type"] = content_type;
        }
        emit_event(after);
        browser_->plugins().dispatch_after_response(request, response);

        AntiBotDetector detector;
        report_anti_bot_detection(detector.inspect_response(request, response));

        const bool redirect = response.status == 301 || response.status == 302 ||
                              response.status == 303 ||
                              response.status == 307 ||
                              response.status == 308;
        const std::string location = response.header("Location");
        if (!redirect || location.empty() ||
            !browser_->config().follow_redirects) {
            if (response.final_url.empty()) {
                response.final_url = request.url;
            }
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
        redirect_event.url = request.url;
        redirect_event.attributes["location"] = location;
        emit_event(redirect_event);
        if (redirect_event.cancelled) {
            response.final_url = request.url;
            response.redirect_chain = chain;
            return response;
        }
        chain.push_back(request.url);
        const std::string redirected_url = resolve_url(request.url, location);
        const Url redirected = parse_url(redirected_url);
        if (!same_origin(parsed, redirected)) {
            request.headers.erase(
                std::remove_if(request.headers.begin(), request.headers.end(),
                               [](const auto& header) {
                                   return sensitive_redirect_header(header.first);
                               }),
                request.headers.end());
        }
        request.url = redirected_url;
        // 303 always reissues as GET; 301/302 also convert non-idempotent
        // methods to GET, matching common browser behavior. 307/308 preserve
        // the method and body.
        if (response.status == 303 ||
            (response.status != 307 && response.status != 308 &&
             request.method != "GET" && request.method != "HEAD")) {
            request.method = "GET";
            request.body.clear();
        }
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
    emit_event(before);
    if (before.cancelled) {
        return;
    }

    const HttpResponse response = request(build_request(url));
    if (response.status >= 400) {
        throw Error(ErrorCode::NetworkError,
                    "navigation failed with status " +
                        std::to_string(response.status) + " for " +
                        std::string(url));
    }
    install_document(response.body, response.final_url,
                     response.final_url);
    follow_script_navigations();
    std::string landed_url = current_url_;

    Event after;
    after.type = EventType::AfterNavigation;
    after.url = landed_url;
    after.attributes["status"] = std::to_string(response.status);
    emit_event(after);
}

void Session::install_document(std::string_view html, std::string url,
                               std::string base_url) {
    page_script_requests_.clear();
    page_script_texts_.clear();
    std::string previous_url = std::move(current_url_);
    document_ = parse_html(html, std::move(url), std::move(base_url));
    current_url_ = document_->url();
    if (script_host_ != nullptr) {
        script_host_->install(document_, document_->base_url());
        script_host_->set_referrer(std::move(previous_url));
    }
    document_->set_mutation_listener([this](const flatworm::MutationInfo& info) {
        Event event;
        event.type = EventType::DomMutation;
        event.url = current_url_;
        event.name = info.kind;
        event.attributes["node"] = info.node_name;
        if (!info.attribute_name.empty()) {
            event.attributes["attribute"] = info.attribute_name;
        }
        // Mutated values are deliberately not forwarded: private form values
        // must stay redacted (README "Security and Resource Limits").
        emit_event(event);
    });

    Event created;
    created.type = EventType::DocumentCreated;
    created.url = current_url_;
    created.name = document_->title();
    created.payload = document_;
    emit_event(created);
    browser_->plugins().dispatch_document(*document_);

    AntiBotDetector detector;
    const AntiBotDetection document_detection =
        detector.inspect_document(*document_);
    if (document_detection.activated && anti_bot_detection_.has_value()) {
        report_anti_bot_detection(
            detector.merge(*anti_bot_detection_, document_detection));
    } else {
        report_anti_bot_detection(document_detection);
    }

    if (javascript_ == nullptr) {
        return;
    }
    if (javascript_->name() == "null") {
        browser_->handle_unsupported_api(
            "javascript", "page scripts were not executed: no JavaScript engine");
        return;
    }
    std::string class_name = document_element_class_name();
    if (!class_name.empty()) {
        std::istringstream stream(class_name);
        std::string token;
        std::string updated;
        bool has_js = false;
        while (stream >> token) {
            if (token == "no-js") {
                continue;
            }
            if (token == "js") {
                has_js = true;
            }
            if (!updated.empty()) {
                updated += ' ';
            }
            updated += token;
        }
        if (!has_js) {
            if (!updated.empty()) {
                updated += ' ';
            }
            updated += "js";
        }
        set_document_element_class_name(updated);
    }
    for (const auto& noscript : document_->query_selector_all("noscript")) {
        auto parent = noscript->parent();
        if (parent != nullptr) {
            parent->remove_child(noscript);
        }
    }
    for (const auto& script : document_->scripts()) {
        std::string script_url;
        const std::string script_text = [&]() -> std::string {
            if (!script->has_attribute("src")) {
                return script->text();
            }
            // External scripts are fetched host-mediated and executed so that
            // pages behave like real browsers and endpoint discovery can see
            // their network calls (README "Parsing HTML and CSS").
            try {
                script_url =
                    resolve_url(base_url.empty() ? current_url_ : base_url,
                                script->attribute("src"));
                const HttpResponse script_response =
                    request(build_request(script_url));
                if (script_response.status >= 200 &&
                    script_response.status < 300 &&
                    script_response.body.size() <= 2u * 1024u * 1024u) {
                    page_script_texts_.push_back(
                        PageScriptText{script_url, script_response.body});
                }
                return script_response.body;
            } catch (const std::exception&) {
                return {};
            }
        }();
        if (script_text.empty()) {
            continue;
        }
        Event begin;
        begin.type = EventType::BeforeScript;
        begin.url = current_url_;
        emit_event(begin);

        const ScriptResult result = javascript_->evaluate(script_text);
        Event end;
        end.type = EventType::AfterScript;
        end.url = current_url_;
        if (!result.ok) {
            end.type = EventType::ScriptException;
            end.message = result.error;
        }
        emit_event(end);
    }
    run_script_lifecycle();
}

void Session::load_html(std::string_view html, std::string_view base_url) {
    if (closed_) {
        throw Error(ErrorCode::InvalidArgument, "session is closed");
    }
    const std::string base =
        base_url.empty() ? current_url_ : std::string(base_url);
    install_document(html, base, base);
    // Offline document installs never navigate away (README "Drivers":
    // deterministic offline runs).
    PendingNavigation discarded;
    while (script_host_ != nullptr &&
           script_host_->consume_pending_navigation(discarded)) {
    }
}

void Session::set_header(std::string name, std::string value) {
    for (auto& [key, existing] : headers_) {
        if (header_name_equal(key, name)) {
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
        emit_event(event);
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
    emit_event(event);
}

}  // namespace prowsetk
