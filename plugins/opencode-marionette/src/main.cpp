#include "prowsetk/plugins/opencode_marionette.hpp"
#include "prowsetk/cookie_import.hpp"
#include "prowsetk/error.hpp"
#include "prowsetk/lua_runtime.hpp"
#include "prowsetk/project_config.hpp"
#include "prowsetk/url.hpp"
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cctype>
#include <fstream>
#include <iostream>
#include <map>
#include <string_view>
namespace marionette = prowsetk::plugins::opencode_marionette;

namespace {
std::string safe_hostname(std::string_view host) {
    if (host.empty() || host.size() > 255 || !std::all_of(host.begin(), host.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '.' || c == '-' || c == ':' || c == '[' || c == ']';
    })) return "unavailable";
    return std::string(host);
}
std::string safe_host(std::string_view value) {
    try { return safe_hostname(prowsetk::parse_url(value).host); }
    catch (...) { return "unavailable"; }
}
// Only fixed, value-free reasons cross the CLI diagnostic boundary. Lua,
// selectors, model answers and filesystem exceptions can contain caller data.
std::string safe_failure(std::string_view error) {
    for (const auto reason : {
        "browser interaction was not confirmed", "Firefox cookie capture failed",
        "Firefox cookie import failed", "Firefox cookie import returned no cookies",
        "Firefox cookie import requires a cookie file and managed session",
        "login was not confirmed", "missing Booking.com credentials",
        "challenge detected", "requires browser JavaScript or captcha support",
        "login form not available", "oauth login failed", "login failed",
        "load_dotenv:", "dotenv loading failed", "TLS certificate verification failed", "TLS handshake failed",
        "HTTPS requires an OpenSSL-enabled build", "DNS resolution failed",
        "connection failed", "proxy CONNECT rejected", "proxy CONNECT failed",
        "SOCKS5 authentication rejected", "SOCKS5 authentication failed",
        "invalid runner options", "invalid decisions JSON", "cannot read decisions file",
        "cookie JSON parse error", "cookie JSON must be an array", "cannot open cookie JSON file",
        "Booking driver failed to load", "Booking preparation returned a nonzero exit code"}) {
        if (error.find(reason) != std::string_view::npos) {
            if (std::string_view(reason) == "load_dotenv:") return "dotenv loading failed";
            return reason;
        }
    }
    for (const auto phase : {"policy validation", "endpoint collection and schema enrichment", "OpenCode session creation",
                             "OpenCode decision", "page action", "schema serialization"}) {
        const auto reason = std::string(phase) + " failed";
        if (error.find(reason) != std::string_view::npos) return reason;
    }
    return "operation failed";
}

class DiagnosticNetwork final : public prowsetk::NetworkClient {
public:
    DiagnosticNetwork(std::unique_ptr<prowsetk::NetworkClient> client, const char* role, bool verbose)
        : client_(std::move(client)), role_(role), verbose_(verbose) {}
    prowsetk::HttpResponse send(const prowsetk::HttpRequest& request) override {
        const auto started = std::chrono::steady_clock::now();
        const auto sequence = ++requests_;
        if (verbose_) {
            const auto selected = request.proxy.enabled() ? request.proxy :
                (client_->proxy().enabled() ? client_->proxy() : prowsetk::proxy_from_environment(request.url));
            const bool cookies = std::any_of(request.headers.begin(), request.headers.end(), [](const auto& header) {
                std::string name = header.first;
                std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::tolower(c); });
                return name == "cookie" && !header.second.empty();
            });
            std::cerr << "opencode-marionette: " << role_ << " request #" << sequence
                      << " host=" << safe_host(request.url) << " route=";
            if (!selected.enabled()) std::cerr << "direct";
            else {
                const char* scheme = selected.scheme == prowsetk::ProxyScheme::Socks5 ? "socks5" :
                    selected.scheme == prowsetk::ProxyScheme::Https ? "https" : "http";
                std::cerr << scheme << "-proxy host=" << safe_hostname(selected.host);
            }
            std::string_view method = "other";
            for (const auto known : {"GET", "HEAD", "POST", "PUT", "PATCH", "DELETE", "OPTIONS"}) {
                if (request.method == known) method = known;
            }
            std::cerr << " cookies=" << (cookies ? "yes" : "no") << " method=" << method << '\n';
        }
        try {
            auto response = client_->send(request);
            if (verbose_) {
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - started).count();
                std::cerr << "opencode-marionette: " << role_ << " response #" << sequence
                          << " HTTP=" << response.status << " bytes=" << response.body.size()
                          << " elapsed-ms=" << elapsed << '\n';
            }
            return response;
        } catch (const prowsetk::Error& error) {
            if (verbose_) std::cerr << "opencode-marionette: " << role_ << " request #" << sequence
                << " failed [" << prowsetk::to_string(error.code()) << "]: " << safe_failure(error.what()) << '\n';
            throw;
        }
    }
    std::string name() const override { return client_->name(); }
    void set_proxy(prowsetk::ProxyConfig proxy) override { client_->set_proxy(std::move(proxy)); }
    prowsetk::ProxyConfig proxy() const override { return client_->proxy(); }
private:
    std::unique_ptr<prowsetk::NetworkClient> client_;
    const char* role_;
    bool verbose_;
    unsigned requests_ = 0;
};

struct DiagnosticEvents {
    prowsetk::EventDispatcher& events;
    prowsetk::SubscriptionId id = 0;
    unsigned script_errors = 0, unsupported = 0;
    DiagnosticEvents(prowsetk::EventDispatcher& dispatcher, bool verbose) : events(dispatcher) {
        if (!verbose) return;
        id = events.subscribe_all([this](prowsetk::Event& event) {
            if (event.type == prowsetk::EventType::ScriptException) {
                ++script_errors;
                if (script_errors <= 20) std::cerr << "opencode-marionette: page JavaScript exception #"
                    << script_errors << " (message omitted)\n";
            } else if (event.type == prowsetk::EventType::UnsupportedApi) ++unsupported;
        });
    }
    ~DiagnosticEvents() {
        if (!id) return;
        events.unsubscribe(id);
        std::cerr << "opencode-marionette: page runtime summary: script-errors=" << script_errors
                  << " unsupported-api-events=" << unsupported << '\n';
    }
};

void usage() {
    std::cerr << "Usage: ptk-opencode-marionette DECISIONS.json URL OPENAPI.yaml POSTMAN.json [HTML_FILE]\n"
                 "  --booking-config Prowse.toml   prepare the same session with Booking login\n"
                 "  --html HTML                   offline HTML (no page network or GET probes)\n"
                 "  --cookies-json FILE --dotenv FILE\n"
                 "  --assistant-browser-force true|false\n"
                 "  --success-beacon QUERY --success-beacon-type TYPE\n"
                 "  --no-xcors true|false          export only booking.com hosts\n"
                 "  --max-steps N --max-page-requests N --max-get-probes N\n"
                 "  --opencode-max-requests N --opencode-wait-ms N\n"
                 "  -v, --verbose                 value-free stage/network/login diagnostics\n"
                 "  --opencode-api-prefix PREFIX   default: /api (OpenCode V2)\n";
}
[[noreturn]] void invalid() {
    throw prowsetk::Error(prowsetk::ErrorCode::InvalidArgument, "invalid runner options");
}
unsigned number(std::string_view value, unsigned cap, bool zero = false) {
    unsigned out = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), out);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || out > cap || (!zero && !out)) invalid();
    return out;
}
bool boolean(std::string_view value) {
    if (value == "true") return true;
    if (value == "false") return false;
    invalid();
}
std::string read_html(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw prowsetk::Error(prowsetk::ErrorCode::IoError, "HTML file unreadable");
    std::string html(16u * 1024u * 1024u + 1u, '\0');
    input.read(html.data(), static_cast<std::streamsize>(html.size()));
    html.resize(static_cast<std::size_t>(input.gcount()));
    if (input.bad() || html.size() > 16u * 1024u * 1024u) throw prowsetk::Error(prowsetk::ErrorCode::ResourceLimit, "HTML limit");
    return html;
}
bool booking_host(const std::string& url) {
    const auto host = prowsetk::parse_url(url).host;
    return host == "booking.com" || host.ends_with(".booking.com");
}
void prepare_booking(prowsetk::Browser& browser, const std::shared_ptr<prowsetk::Session>& session,
                     const prowsetk::ProjectConfig& config, const std::filesystem::path& config_path,
                     const std::map<std::string, std::string>& provided, const std::string& cookies) {
    if (!prowsetk::LuaRuntime::available()) throw prowsetk::Error(prowsetk::ErrorCode::Unsupported, "Lua required for Booking login");
    const auto driver = std::find_if(config.drivers.begin(), config.drivers.end(),
        [](const auto& item) { return item.name == "booking-dotcom-admin"; });
    if (driver == config.drivers.end() || !driver->enabled || driver->script.empty()) invalid();
    std::vector<prowsetk::LuaArgument> args;
    for (const auto& declared : driver->arguments) {
        const auto found = provided.find(declared.name);
        if (found != provided.end()) args.push_back({declared.name, declared.type, found->second});
        else if (!declared.default_value.empty()) args.push_back({declared.name, declared.type, declared.default_value});
        else if (declared.required) invalid();
    }
    for (const auto& entry : provided) {
        if (std::none_of(driver->arguments.begin(), driver->arguments.end(),
            [&](const auto& item) { return item.name == entry.first; })) invalid();
    }
    args.push_back({"cookies_json", "path", cookies});
    args.push_back({"assistant_browser_enabled", "boolean", config.assistant_browser.enabled ? "true" : "false"});
    args.push_back({"assistant_browser", "string", config.assistant_browser.command});
    args.push_back({"assistant_browser_method", "string", config.assistant_browser.method});
    const auto root = config_path.parent_path() / config.root;
    prowsetk::LuaRuntime lua;
    lua.bind_browser(&browser);
    lua.bind_session(session);
    std::string code;
    if (!lua.run_file((root / driver->script).string()).ok)
        throw prowsetk::Error(prowsetk::ErrorCode::LuaError, "Booking driver failed to load");
    const auto prepared = lua.call_function("prepare_session", args, &code);
    if (!prepared.ok) throw prowsetk::Error(prowsetk::ErrorCode::LuaError, safe_failure(prepared.error));
    if (code != "0") throw prowsetk::Error(prowsetk::ErrorCode::LuaError, "Booking preparation returned a nonzero exit code");
}
}

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--help") { usage(); return 0; }
    if (argc < 5) {
        usage();
        return 2;
    }
    std::string_view stage = "configuration";
    try {
        auto decisions = marionette::load_decisions(argv[1]);
        auto bridge_config = prowsetk::plugins::opencode_bridge::config_from_environment();
        bridge_config.api_prefix = "/api";
        bridge_config.max_requests = 512;
        bridge_config.prompt_wait_ms = 300000;
        std::filesystem::path booking_config;
        std::string cookies, html;
        bool offline = false, no_xcors = false, verbose = false;
        std::map<std::string, std::string> args{{"url", argv[2]}};
        int at = 5;
        if (at < argc && !std::string_view(argv[at]).starts_with("-")) {
            html = read_html(argv[at++]);
            offline = true;
        }
        while (at < argc) {
            const std::string option = argv[at++];
            if (option == "--verbose" || option == "--verbse" || option == "-v") {
                verbose = true;
                args["verbose"] = "true";
                continue;
            }
            if (at >= argc) invalid();
            const std::string value = argv[at++];
            if (option == "--booking-config") booking_config = value;
            else if (option == "--cookies-json") cookies = value;
            else if (option == "--html") {
                if (offline || value.empty()) invalid();
                html = value;
                offline = true;
            } else if (option == "--dotenv") args["dotenv"] = value;
            else if (option == "--success-beacon") args["success_beacon"] = value;
            else if (option == "--success-beacon-type") args["success_beacon_type"] = value;
            else if (option == "--assistant-browser-force") {
                boolean(value);
                args["assistant_browser_force"] = value;
            } else if (option == "--no-xcors") {
                no_xcors = boolean(value);
                args["no-xcors"] = value;
            } else if (option == "--max-steps") decisions.max_steps = number(value, 64);
            else if (option == "--max-page-requests") decisions.max_page_requests = number(value, 1024);
            else if (option == "--max-get-probes") decisions.max_get_probes = number(value, 128, true);
            else if (option == "--opencode-max-requests") bridge_config.max_requests = number(value, 1024);
            else if (option == "--opencode-wait-ms") bridge_config.prompt_wait_ms = static_cast<int>(number(value, 600000));
            else if (option == "--opencode-api-prefix") bridge_config.api_prefix = value;
            else invalid();
        }
        if (offline && html.empty()) invalid();
        // Check transport settings before login or browser interaction.
        prowsetk::plugins::opencode_bridge::validate_config(bridge_config);
        prowsetk::ProjectConfig project;
        if (!booking_config.empty()) project = prowsetk::load_project_config(booking_config.string());
        prowsetk::BrowserConfig config;
        if (!booking_config.empty()) {
            config.javascript = project.javascript;
            if (!project.user_agent.empty()) config.user_agent = project.user_agent;
            config.follow_redirects = project.follow_redirects;
            config.max_redirects = project.max_redirects;
            config.timeout_ms = project.timeout_ms;
            if (!project.network_proxy.empty()) config.proxy = prowsetk::parse_proxy_url(project.network_proxy);
            if (cookies.empty() && !project.sessions.cookies_json.empty()) {
                cookies = (booking_config.parent_path() / project.root / project.sessions.cookies_json).string();
            }
        }
        config.observe_network = true;
        prowsetk::Browser browser(config);
        if (offline) {
            // Deterministic page transport; only the separate agent client is live.
            browser.set_network_client(std::make_unique<prowsetk::MemoryNetworkClient>());
            decisions.max_get_probes = 0;
            args["html"] = html;
        }
        if (verbose && !offline) browser.set_network_client(std::make_unique<DiagnosticNetwork>(
            prowsetk::make_socket_network_client(), "page", true));
        DiagnosticEvents diagnostics(browser.events(), verbose);
        stage = "cookie import";
        if (!cookies.empty() && std::filesystem::exists(cookies)) {
            const auto imported = prowsetk::import_cookies_json_file(browser.storage().cookies(), cookies, argv[2]);
            if (verbose) std::cerr << "opencode-marionette: initial cookies imported=" << imported.imported
                                  << " skipped=" << imported.skipped << " (values omitted)\n";
        } else if (!cookies.empty() && booking_config.empty()) {
            throw prowsetk::Error(prowsetk::ErrorCode::IoError, "cookie file unreadable");
        }
        auto session = browser.create_session();
        stage = "session preparation";
        if (verbose) std::cerr << "opencode-marionette: preparing " << (offline ? "offline" : "live")
                               << " session (before OpenCode exploration)\n";
        if (!booking_config.empty()) prepare_booking(browser, session, project, booking_config, args, cookies);
        else if (offline) session->load_html(html, argv[2]);
        else session->navigate(argv[2]);
        // OpenCode uses its own host transport, never the page session's auth.
        stage = "OpenCode exploration";
        if (verbose) std::cerr << "opencode-marionette: session ready; exploration budgets: actions=" << decisions.max_steps
            << " page-requests=" << decisions.max_page_requests << " GET-probes=" << decisions.max_get_probes << '\n';
        auto network = std::make_unique<DiagnosticNetwork>(prowsetk::make_socket_network_client(), "OpenCode", verbose);
        prowsetk::plugins::opencode_bridge::OpenCodeClient client(*network, bridge_config);
        auto result = marionette::run(*session, client, decisions);
        if (no_xcors) {
            std::erase_if(result.extraction.schemas, [](const auto& item) { return !booking_host(item.endpoint.url); });
            prowsetk::plugins::schema_grabber::SchemaGrabberOptions options;
            options.require_api_pattern = false;
            options.include_examples = false;
            const prowsetk::Redactor redactor;
            result.extraction.openapi_yaml = prowsetk::plugins::schema_grabber::render_schema_yaml(result.extraction.schemas, options, redactor);
            result.extraction.openapi_yaml += "x-prowsetk-marionette:\n  used: true\n  coverage-complete: false\n  steps: " +
                std::to_string(result.steps) + "\n  reason: " + result.reason + "\n  get-probes: " +
                std::to_string(result.extraction.probe_count) + "\n";
            result.extraction.postman_json = prowsetk::plugins::schema_grabber::render_schema_postman_json(result.extraction.schemas, options, redactor);
        }
        if (!booking_config.empty()) result.extraction.openapi_yaml +=
            std::string("x-prowsetk-booking:\n  authenticated: ") + (offline ? "false\n" : "true\n");
        stage = "export";
        if (verbose) std::cerr << "opencode-marionette: exporting discovered schemas\n";
        for (const auto* path : {argv[3], argv[4]}) {
            const auto parent = std::filesystem::path(path).parent_path();
            if (!parent.empty()) std::filesystem::create_directories(parent);
        }
        result.extraction.write_openapi_yaml(argv[3]);
        result.extraction.write_postman_json(argv[4]);
        std::cout << "Discovered " << result.extraction.schemas.size() << " endpoint schemas; "
                  << result.steps << " actions; " << result.reason << "; coverage incomplete\n";
        return 0;
    } catch (const prowsetk::Error& error) {
        std::cerr << "opencode-marionette: " << stage << " failed [" << prowsetk::to_string(error.code())
                  << "]: " << safe_failure(error.what()) << '\n';
        return 1;
    } catch (...) {
        std::cerr << "opencode-marionette: " << stage << " failed [internal]\n";
        return 1;
    }
}
