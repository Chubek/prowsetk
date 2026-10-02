#include "support.hpp"
#include "lua_sources.hpp"
#include <prowsetk/cookie_import.hpp>
#include <prowsetk/project_config.hpp>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <poll.h>
#include <unistd.h>

namespace pa = prowsetk::automation;
namespace fs = std::filesystem;
namespace {
fs::path relative(const fs::path& root, const std::string& value) {
    return fs::path(value).is_absolute() ? fs::path(value) : root / value;
}
bool assistant(const toml::table& config, const fs::path& root, const std::string& url, const char* executable) {
    const auto* section = config["assistant"].as_table();
    if (!section || !pa::bool_setting(*section, "enabled", false)) return false;
    pa::check_keys(*section, {"enabled", "project", "driver", "command", "wait_timeout_ms"});
    std::cerr << "Crawler needs browser assistance. Run the configured assistant? [y/N] " << std::flush;
    std::string approval;
    if (!std::getline(std::cin, approval) || (approval != "y" && approval != "Y")) return false;
    const int timeout = pa::int_setting(*section, "wait_timeout_ms", 300000, 1, 3600000);
    const auto project = pa::string_setting(*section, "project");
    if (!project.empty()) {
        // A PATH invocation has no directory in argv[0]; let execvp resolve
        // the installed CLI from PATH too. Explicit paths prefer a sibling.
        const fs::path invoked(executable);
        fs::path cli = invoked.has_parent_path() ? fs::absolute(invoked).parent_path() / "prowsetk" : fs::path("prowsetk");
#ifdef PROWSETK_CLI_BUILD_PATH
        if (invoked.has_parent_path() && !fs::exists(cli)) cli = PROWSETK_CLI_BUILD_PATH;
#endif
        if (pa::run_process({cli.string(), "run", pa::string_setting(*section, "driver", "assist"), "--config", relative(root, project).string(), "--url", url}, timeout) != 0) return false;
    } else {
        const char* override = std::getenv("PROWSETK_ASSISTANT_BROWSER");
        const auto command = override ? std::string(override) : pa::string_setting(*section, "command", "assistant-browser");
        if (pa::run_process({command, url}, timeout) != 0) return false;
        std::cerr << "Finish browser interaction and export fresh cookies to the configured cookie file, then press Enter.\n";
        pollfd input{STDIN_FILENO, POLLIN, 0};
        if (::poll(&input, 1, timeout) <= 0 || !std::getline(std::cin, approval)) return false;
    }
    return true;
}
}
int main(int argc, char** argv) {
    try {
        fs::path path = "Crawler.toml";
        std::optional<std::string> html;
        std::string output_override;
        for (int i = 1; i < argc; ++i) {
            const std::string option = argv[i];
            if (option == "--help") { std::cout << "usage: crawler --config Crawler.toml [--html HTML | --html-file FILE] [--output FILE]\n"; return 0; }
            if (i + 1 == argc) throw std::runtime_error("missing option value");
            if (option == "--config") path = argv[++i];
            else if (option == "--html") html = argv[++i];
            else if (option == "--html-file") html = pa::read_file(argv[++i], 16 * 1024 * 1024);
            else if (option == "--output") output_override = argv[++i];
            else throw std::runtime_error("unknown option");
        }
        const auto config = toml::parse(pa::read_file(path));
        pa::check_keys(config, {"crawler", "login", "engine", "assistant", "arguments"});
        const auto* crawl = config["crawler"].as_table();
        if (!crawl) throw std::runtime_error("missing [crawler] table");
        pa::check_keys(*crawl, {"url", "driver", "query", "max_pages", "max_depth", "max_frontier", "max_requests", "respect_robots", "blocked_paths", "output", "openapi", "postman", "cookies_json", "api_only"});
        const auto root = fs::absolute(path).parent_path();
        const auto url = pa::string_setting(*crawl, "url");
        std::vector<std::string> origins{pa::origin(url)};
        std::vector<prowsetk::LuaArgument> args{
            {"url", "url", url}, {"query", "string", pa::string_setting(*crawl, "query", "select tag, text from <h*>")},
            {"max_pages", "integer", std::to_string(pa::int_setting(*crawl, "max_pages", 100, 1, 10000))},
            {"max_depth", "integer", std::to_string(pa::int_setting(*crawl, "max_depth", 2, 0, 32))},
            {"max_frontier", "integer", std::to_string(pa::int_setting(*crawl, "max_frontier", 1000, 1, 100000))},
            {"respect_robots", "boolean", pa::bool_setting(*crawl, "respect_robots", true) ? "true" : "false"},
            {"api_only", "boolean", pa::bool_setting(*crawl, "api_only", true) ? "true" : "false"}
        };
        if (const auto* blocked = (*crawl)["blocked_paths"].as_array()) {
            std::string patterns;
            for (const auto& node : *blocked) { const auto value = node.value<std::string>(); if (!value) throw std::runtime_error("invalid blocked path"); patterns += *value + "\n"; }
            args.push_back({"blocked_paths", "string", patterns});
        } else if (crawl->contains("blocked_paths")) throw std::runtime_error("blocked_paths must be an array");
        if (const auto* login = config["login"].as_table()) {
            pa::check_keys(*login, {"mode", "url", "success_selector", "username_env", "password_env", "token_env", "value_env", "cookie_env", "as_token_env", "header_name", "username_field", "password_field", "csrf_field", "trusted_origins", "dotenv"});
            std::string trusted;
            if (const auto* list = (*login)["trusted_origins"].as_array()) for (const auto& entry : *list) {
                const auto value = entry.value<std::string>();
                if (!value) throw std::runtime_error("invalid trusted origin");
                origins.push_back(pa::origin(*value)); trusted += origins.back() + "\n";
            }
            args.push_back({"trusted_origins", "string", trusted});
            for (const auto& [key, value] : *login) {
                if (key.str() == "trusted_origins") continue;
                const auto text = value.value<std::string>();
                if (!text) throw std::runtime_error("login fields must be strings");
                auto name = std::string(key.str());
                if (name == "mode") name = "login_mode";
                if (name == "url") { name = "login_url"; origins.push_back(pa::origin(*text)); }
                args.push_back({name, "string", key.str() == "dotenv" ? relative(root, *text).string() : *text});
            }
        }
        if (const auto* extra = config["arguments"].as_table()) {
            for (const auto& arg : pa::scalar_arguments(*extra)) {
                if (std::any_of(args.begin(), args.end(), [&](const auto& existing) { return existing.name == arg.name; }) || arg.name == "html") throw std::runtime_error("reserved driver argument");
                args.push_back(arg);
            }
        }
        if (html) args.push_back({"html", "string", *html});
        const auto engine = config["engine"].as_table();
        const auto browser_config = pa::browser_config(engine ? *engine : toml::table{}, html.has_value());
        const auto cookies = pa::string_setting(*crawl, "cookies_json");
        const auto driver = pa::string_setting(*crawl, "driver");
        for (int attempt = 0; attempt < 2; ++attempt) {
            prowsetk::Browser browser(browser_config);
            browser.set_network_client(std::make_unique<pa::RestrictedNetwork>(html ? std::vector<std::string>{} : origins,
                static_cast<std::size_t>(pa::int_setting(*crawl, "max_requests", 1000, 1, 100000))));
            if (!html && !cookies.empty() && fs::exists(relative(root, cookies))) prowsetk::import_cookies_json_file(browser.storage().cookies(), relative(root, cookies));
            auto session = browser.create_session();
            prowsetk::LuaRuntime lua;
            lua.bind_browser(&browser); lua.bind_session(session);
            pa::load_module(lua, "ezlogin", pa::ezlogin_source);
            pa::load_module(lua, "scrape_endpoints", pa::scrape_endpoints_source);
            pa::load_module(lua, "lcrawler", pa::lcrawler_source);
            const auto script = driver.empty() ? "function main(args) return require('lcrawler').run(session,args) end" : pa::read_file(relative(root, driver));
            if (!lua.run(script, "crawler-driver").ok) throw std::runtime_error("cannot load crawler driver");
            const auto code = pa::call(lua, "main", args);
            if (code == "3" && attempt == 0 && !html && assistant(config, root, url, argv[0])) continue;
            if (code != "0") { std::cerr << "crawler: " << (code == "3" ? "authentication or browser assistance required" : "crawl failed") << '\n'; return 1; }
            const auto output = output_override.empty() ? relative(root, pa::string_setting(*crawl, "output", "output/pages.jsonl")) : fs::path(output_override);
            if (!output.parent_path().empty()) fs::create_directories(output.parent_path());
            pa::write_file(output, pa::call(lua, "__crawler_result", {{"kind", "string", "pages"}}));
            for (const auto* name : {"openapi", "postman"}) {
                const auto destination = pa::string_setting(*crawl, name);
                if (!destination.empty()) { const auto target = relative(root, destination); fs::create_directories(target.parent_path()); pa::write_file(target, pa::call(lua, "__crawler_result", {{"kind", "string", name}})); }
            }
            std::cout << pa::call(lua, "__crawler_result", {{"kind", "string", "summary"}}) << '\n';
            return 0;
        }
        return 1;
    } catch (const std::exception&) { std::cerr << "crawler: configuration, driver or I/O failure\n"; return 1; }
}
