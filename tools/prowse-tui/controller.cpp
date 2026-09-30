#include "model.hpp"
#include <prowsetk/endpoint_extraction.hpp>
#include <prowsetk/lua_runtime.hpp>
#include <prowsetk/url.hpp>
#include <fstream>
#include <algorithm>
#include <sstream>
#include <set>
#include <regex.h>
#include <stdexcept>
namespace prowsetk::tui {
namespace {
BrowserConfig config() {
    BrowserConfig c;
    c.html_parser = HtmlParser::Auto;
    c.navigation_javascript_only = true;
    return c;
}
void write_file(const std::string& path, const std::string& content) {
    if (path.empty()) throw std::runtime_error("Output path required");
    std::ofstream file(path, std::ios::binary);
    file << content;
    if (!file) throw std::runtime_error("Cannot write output file");
}
std::string export_url(std::string_view raw) {
    auto url = parse_url(raw);
    url.userinfo.clear(); url.fragment.clear(); url.has_fragment = false;
    return Redactor{}.redact_url(url.to_string());
}
}
Controller::Controller() : browser(config()), session(browser.create_session()) {
    if (browser.lua()) browser.lua()->bind_session(session);
    session->load_html("<h1>Prowse-TUI</h1><p>Press : for commands, :help for help.</p>", "https://localhost/");
    refresh();
}
void Controller::refresh(std::size_t width) {
    width_ = width;
    help.resize(width);
    page = render(emit_prowse_events(*session->document()), session->document()->base_url(), width);
    if (top >= page.lines.size()) top = page.lines.empty() ? 0 : page.lines.size() - 1;
}
void Controller::navigate(std::string_view input) {
    auto url = resolve_url(session->current_url(), input);
    auto parsed = parse_url(url);
    if (parsed.scheme != "http" && parsed.scheme != "https") throw std::runtime_error("Only HTTP(S) navigation is supported");
    const auto old = session->current_url();
    session->navigate(url);
    if (history_.size() == 256) history_.erase(history_.begin());
    history_.push_back(old);
    top = 0;
    view = ViewMode::Browser;
}
std::string Controller::command(std::string_view input) {
    if (input.size() > 65536) throw std::runtime_error("Command too long");
    std::istringstream in{std::string(input)};
    std::string name, rest;
    in >> name;
    std::getline(in >> std::ws, rest);
    std::string result = "OK";
    if (name == "quit" || name == "q") quit = true;
    else if (name == "help") {
        if (rest.empty()) help.open("overview");
        else if (rest == "list") help.index();
        else if (rest.starts_with("open ")) help.open(rest.substr(5));
        else if (rest.starts_with("find ")) help.find(rest.substr(5));
        else throw std::runtime_error("Use :help, :help list, :help open PAGE or :help find REGEX");
        view = ViewMode::Help;
        return {};
    } else if (name == "config") return config_command(rest);
    else if (name == "open") navigate(rest);
    else if (name == "follow") {
        std::size_t used = 0;
        auto n = std::stoul(rest, &used);
        if (used != rest.size() || n == 0 || n > page.links.size()) throw std::runtime_error("Invalid link number");
        navigate(page.links[n - 1]);
    } else if (name == "back") {
        if (history_.empty()) throw std::runtime_error("History empty");
        session->navigate(history_.back()); history_.pop_back(); top = 0;
    } else if (name == "reload") session->navigate(session->current_url());
    else if (name == "top") top = 0;
    else if (name == "bottom") top = page.lines.empty() ? 0 : page.lines.size() - 1;
    else if (name == "bookmark") { bookmarks_.push_back(session->current_url()); result = "Bookmarked"; }
    else if (name == "bookmarks") { result.clear(); for (const auto& u : bookmarks_) result += export_url(u) + "\n"; }
    else if (name == "links" || name == "urls") {
        result.clear(); std::set<std::string> seen;
        auto urls = page.links;
        for (const auto& u : session->document()->resource_urls()) urls.push_back(resolve_url(session->document()->base_url(), u));
        for (const auto& u : urls) if (seen.insert(u).second) result += export_url(u) + "\n";
        if (name == "urls") { write_file(rest, result); result = "Saved redacted URLs"; }
    } else if (name == "text") {
        result.clear(); for (const auto& line : page.lines) result += line + "\n";
        write_file(rest, result); result = "Saved visible text";
    } else if (name == "endpoints") {
        EndpointExtractor extractor;
        for (const auto& req : session->page_script_requests()) extractor.observe(req.method, req.url, req.status, req.content_type);
        for (const auto& script : session->page_script_texts()) extractor.observe_script(script.url, script.body);
        write_file(rest, extractor.extract(*session->document()).openapi_yaml);
        result = "Saved heuristic OpenAPI (coverage incomplete)";
    } else if (name == "lua" || name == "source" || name == "driver") {
        auto* lua = browser.lua();
        if (!lua || !LuaRuntime::available()) throw std::runtime_error("Lua unavailable");
        auto r = name == "lua" ? lua->run(rest) : lua->run_file(rest);
        if (!r.ok) throw std::runtime_error("Lua execution failed");
        if (name == "driver") {
            std::string code;
            r = lua->call_function("main", {}, &code);
            if (!r.ok || code != "0") throw std::runtime_error("Driver failed");
        }
    } else if (name == "find") {
        if (rest.empty() || rest.size() > 1024) throw std::runtime_error("Regex must contain 1..1024 bytes");
        regex_t regex{};
        if (regcomp(&regex, rest.c_str(), REG_EXTENDED | REG_NOSUB)) throw std::runtime_error("Invalid regex");
        struct Cleanup { regex_t* p; ~Cleanup() { regfree(p); } } cleanup{&regex};
        bool found = false;
        for (std::size_t n = 1; n <= page.lines.size(); ++n) {
            auto i = (top + n) % page.lines.size();
            if (regexec(&regex, page.lines[i].c_str(), 0, nullptr, 0) == 0) { top = i; found = true; break; }
        }
        result = found ? "Match" : "No match";
    } else if (name == "select") {
        const auto nodes = session->document()->query_selector_all(rest);
        result = std::to_string(nodes.size()) + " elements";
        for (const auto& node : nodes) result += "\n" + node->tag_name() + ": " + safe_text(node->text());
    } else if (name == "click") {
        auto node = session->document()->query_selector(rest);
        if (!node || !session->click_element(node)) throw std::runtime_error("Element cannot be clicked");
    } else if (name == "set") {
        auto split = rest.find(' ');
        if (split == std::string::npos) throw std::runtime_error("set requires selector and value");
        auto node = session->document()->query_selector(rest.substr(0, split));
        if (!node || !session->type_element(node, rest.substr(split + 1))) throw std::runtime_error("Element cannot be edited");
    } else throw std::runtime_error("Unknown command; use help");
    refresh(width_);
    return result;
}

void Controller::initialize_settings(const std::filesystem::path& path) {
    config_path = path;
    const auto loaded = load_settings(path);
    if (!loaded.proxy.empty()) browser.network_client().set_proxy(parse_proxy_url(loaded.proxy));
    settings = loaded; draft_settings = loaded; config_dirty = false;
    for (const auto& plugin : settings.native_plugins) {
        try { browser.plugins().load_native(extension_path(path, plugin)); }
        catch (const std::exception&) { throw std::runtime_error("Configured native plugin could not be loaded; edit :config and restart"); }
    }
    try { browser.plugins().initialize_all(); browser.plugins().configure_all(); }
    catch (const std::exception&) { throw std::runtime_error("Configured native plugin initialization failed"); }
    for (const auto& extension : settings.lua_extensions) {
        auto* lua = browser.lua();
        if (!lua || !LuaRuntime::available()) throw std::runtime_error("Configured Lua extension requires Lua support");
        const auto result = lua->run_file(extension_path(path, extension).string());
        if (!result.ok) throw std::runtime_error("Configured Lua extension failed; edit :config and restart");
    }
}
std::vector<ConfigRow> Controller::config_rows() const {
    std::vector<ConfigRow> rows;
    rows.push_back({"Save changes (keys apply now; extensions on next launch)", "config save"});
    rows.push_back({"Reload from disk / discard unsaved edits", "config reload"});
    for (const auto& [action, key] : draft_settings.keys)
        rows.push_back({"Key: " + action + " = " + key, "config bind " + action + " "});
    rows.push_back({"Add Lua extension...", "config add-lua "});
    for (std::size_t i = 0; i < draft_settings.lua_extensions.size(); ++i)
        rows.push_back({"Remove Lua " + std::to_string(i + 1) + ": " + draft_settings.lua_extensions[i], "config remove-lua " + std::to_string(i + 1)});
    rows.push_back({"Add native plugin...", "config add-plugin "});
    for (std::size_t i = 0; i < draft_settings.native_plugins.size(); ++i)
        rows.push_back({"Remove plugin " + std::to_string(i + 1) + ": " + draft_settings.native_plugins[i], "config remove-plugin " + std::to_string(i + 1)});
    return rows;
}
std::string Controller::config_command(std::string_view input) {
    if (config_path.empty()) config_path = default_settings_path();
    if (input.empty()) { view = ViewMode::Config; return {}; }
    std::istringstream in{std::string(input)};
    std::string verb, rest; in >> verb; std::getline(in >> std::ws, rest);
    if (verb == "save") {
        if (!rest.empty()) throw std::runtime_error("config save takes no arguments");
        save_settings(config_path, draft_settings);
        settings = draft_settings; config_dirty = false;
    } else if (verb == "reload") {
        if (!rest.empty()) throw std::runtime_error("config reload takes no arguments");
        auto loaded = load_settings(config_path);
        settings = loaded; draft_settings = loaded; config_dirty = false;
    } else {
        Settings next = draft_settings;
        if (verb == "bind") {
            std::istringstream binding(rest);
            std::string action, key, extra; binding >> action >> key;
            if (action.empty() || key.empty() || binding >> extra) throw std::runtime_error("Use config bind ACTION KEY");
            next.bind(action, key);
        } else if (verb == "proxy") { next.proxy = rest; if (!rest.empty()) parse_proxy_url(rest);
    } else if (verb == "add-lua") next.lua_extensions.push_back(rest);
        else if (verb == "add-plugin") next.native_plugins.push_back(rest);
        else if (verb == "remove-lua" || verb == "remove-plugin") {
            auto& paths = verb == "remove-lua" ? next.lua_extensions : next.native_plugins;
            std::size_t used = 0, n = 0;
            try { n = std::stoul(rest, &used); } catch (const std::exception&) { throw std::runtime_error("Invalid extension number"); }
            if (used != rest.size() || n == 0 || n > paths.size()) throw std::runtime_error("Invalid extension number");
            paths.erase(paths.begin() + static_cast<std::ptrdiff_t>(n - 1));
        } else throw std::runtime_error("Unknown config command; use :help open config");
        next.validate(); draft_settings = std::move(next); config_dirty = true;
    }
    view = ViewMode::Config;
    config_cursor = std::min(config_cursor, config_rows().size() - 1);
    return {};
}
std::vector<std::string> Controller::visible_lines() const {
    if (view == ViewMode::Browser) return page.lines;
    std::vector<std::string> result;
    if (view == ViewMode::Help) for (const auto& line : help.lines()) result.push_back(line.text);
    else for (const auto& row : config_rows()) result.push_back(row.text);
    return result;
}
void Controller::close_view() { view = ViewMode::Browser; }
std::string Controller::activate_view() {
    if (view == ViewMode::Help) {
        const auto url = help.activate();
        if (!url.empty()) navigate(url);
    } else if (view == ViewMode::Config) {
        const auto rows = config_rows();
        if (config_cursor < rows.size()) return rows[config_cursor].command;
    }
    return {};
}
std::string Controller::key_action(std::string_view action, int page_size) {
    if (action == "help") return command("help");
    if (action == "config") return command("config");
    if (action == "close") { if (view == ViewMode::Browser) quit = true; else close_view(); return {}; }
    if (action == "activate") return activate_view();
    if (action == "back") { if (view == ViewMode::Help) help.back(); else if (view == ViewMode::Config) close_view(); else return command("back"); return {}; }
    if (action == "next_match" || action == "previous_match") {
        if (view == ViewMode::Help) help.next_match(action == "previous_match");
        return {};
    }
    const int delta = action == "down" ? 1 : action == "up" ? -1 : action == "page_down" ? std::max(1, page_size) : action == "page_up" ? -std::max(1, page_size) : 0;
    if (view == ViewMode::Help) {
        if (action == "home") help.home(); else if (action == "end") help.end(); else help.move(delta);
    } else {
        auto& cursor = view == ViewMode::Config ? config_cursor : top;
        const auto size = view == ViewMode::Config ? config_rows().size() : page.lines.size();
        const auto last = size ? size - 1 : 0;
        if (action == "home") cursor = 0;
        else if (action == "end") cursor = last;
        else if (delta < 0) cursor -= std::min(cursor, static_cast<std::size_t>(-delta));
        else cursor = std::min(last, cursor + static_cast<std::size_t>(delta));
    }
    return {};
}
}
