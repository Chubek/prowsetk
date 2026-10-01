#include <prowsetk/plugins/spider.hpp>
#include <prowsetk/error.hpp>
#include <prowsetk/lua_runtime.hpp>
#include <prowsetk/url.hpp>
#include <prowsetk/xpath.hpp>
#include "lspider_source.hpp"
#include <algorithm>
#include <charconv>
#include <deque>
#include <fstream>
#include <set>
#include <sstream>

namespace prowsetk::plugins::spider {
namespace {
using Clock = std::chrono::steady_clock;
std::size_t epoch_seconds() {
    return static_cast<std::size_t>(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}
std::size_t integer(std::string_view text, std::size_t maximum) {
    std::size_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value > maximum)
        throw Error(ErrorCode::InvalidArgument, "invalid integer");
    return value;
}
std::string canonical(std::string_view text) {
    if (text.size() > 4096 || text.find('\0') != std::string_view::npos)
        throw Error(ErrorCode::InvalidUrl, "invalid URL");
    auto url = parse_url(normalize_url(text));
    if (!url.has_host() || (url.scheme != "https" && url.scheme != "http") || !url.userinfo.empty())
        throw Error(ErrorCode::SecurityViolation, "HTTP(S) origin required");
    url.fragment.clear(); url.has_fragment = false;
    if (url.path.empty()) url.path = "/";
    return url.to_string();
}
class OriginNetwork final : public NetworkClient {
public:
    OriginNetwork(std::unique_ptr<NetworkClient> inner, const std::string* origin)
        : inner_(std::move(inner)), origin_(origin) {}
    HttpResponse send(const HttpRequest& request) override {
        const auto url = canonical(request.url);
        if (origin_->empty() || parse_url(url).origin() != *origin_)
            throw Error(ErrorCode::SecurityViolation, "outside spider origin");
        auto bounded = request;
        bounded.timeout_ms = std::clamp(request.timeout_ms, 1, 10000);
        bounded.max_response_bytes = std::min(request.max_response_bytes, std::size_t{2u * 1024u * 1024u});
        return inner_->send(bounded);
    }
private:
    std::unique_ptr<NetworkClient> inner_;
    const std::string* origin_;
};
std::string lua_string(std::string_view value) {
    constexpr char hex[] = "0123456789abcdef";
    std::string out = "\"";
    for (unsigned char c : value) { out += "\\x"; out += hex[c >> 4]; out += hex[c & 15]; }
    return out + '"';
}
std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return text;
}
bool sensitive(std::string_view key) {
    const auto name = lower(std::string(key));
    return Redactor{}.is_sensitive_query_parameter(name) ||
        Redactor{}.is_sensitive_header(name) || name.find("password") != std::string::npos ||
        name.find("token") != std::string::npos || name.find("secret") != std::string::npos ||
        name.find("cookie") != std::string::npos;
}
std::string sanitized_html(const Document& document) {
    auto copy = parse_html(document.html());
    for (const auto& element : copy->query_selector_all("*")) {
        const auto tag = element->tag_name();
        if (tag == "script" || tag == "style" || tag == "textarea") element->set_text("");
        for (const auto& attr : element->attributes()) {
            if (sensitive(attr.name) || attr.name == "value" || attr.name.starts_with("on"))
                element->remove_attribute(attr.name);
            else if (attr.name == "href" || attr.name == "src" || attr.name == "action")
                element->set_attribute(attr.name, Redactor{}.redact_url(attr.value));
        }
    }
    return copy->html();
}
std::string page_json(const std::string& value) {
    const auto fields = unpack(value);
    if (fields.size() != 5) throw Error(ErrorCode::StorageError, "invalid page record");
    return "{\"url\":" + json_quote(fields[0]) + ",\"title\":" + json_quote(fields[1]) +
        ",\"status\":" + fields[2] + ",\"html\":" + json_quote(fields[3]) +
        ",\"fetched_at\":" + fields[4] + ",\"content_omitted\":" + (fields[3].empty() ? "true" : "false") + "}";
}
std::string page_key(std::size_t number) {
    auto digits = std::to_string(number % 256);
    return "page/" + std::string(3 - digits.size(), '0') + digits;
}
// Restricted robots matcher: wildcard '*' and terminal '$', bytewise paths.
bool robot_match(std::string pattern, std::string_view path) {
    const bool exact = pattern.ends_with('$');
    if (exact) pattern.pop_back(); else pattern += '*';
    std::size_t p = 0, s = 0, star = std::string::npos, retry = 0;
    while (s < path.size()) {
        if (p < pattern.size() && pattern[p] == path[s]) { ++p; ++s; }
        else if (p < pattern.size() && pattern[p] == '*') { star = p++; retry = s; }
        else if (star != std::string::npos) { p = star + 1; s = ++retry; }
        else return false;
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}
struct RobotRule { bool allow; std::string path; };
std::vector<RobotRule> parse_robots(std::string_view text) {
    std::vector<RobotRule> general, specific;
    std::istringstream lines{std::string(text)};
    std::string line;
    bool any = false, agent = false, rules_started = false, specific_group = false;
    std::size_t count = 0;
    while (std::getline(lines, line) && ++count <= 2000) {
        line.resize(std::min(line.size(), line.find('#')));
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        auto key = lower(line.substr(0, colon)), value = line.substr(colon + 1);
        auto trim = [](std::string& s) {
            const auto start = s.find_first_not_of(" \t\r");
            if (start == std::string::npos) { s.clear(); return; }
            s = s.substr(start, s.find_last_not_of(" \t\r") - start + 1);
        };
        trim(key); trim(value);
        if (key == "user-agent") {
            if (rules_started) { any = false; agent = false; rules_started = false; }
            any = any || value == "*";
            agent = agent || lower(value) == "prowsetkspider";
            specific_group = specific_group || agent;
        } else if (key == "allow" || key == "disallow") {
            rules_started = true;
            if (value.empty()) continue;
            if (value.size() > 2048 && (agent || any))
                throw Error(ErrorCode::ResourceLimit, "robots rule too large");
            if (agent) specific.push_back({key == "allow", value});
            else if (any) general.push_back({key == "allow", value});
        }
    }
    if (count > 2000) throw Error(ErrorCode::ResourceLimit, "robots rules too large");
    return specific_group ? specific : general;
}
}

struct Engine::Impl {
    struct Task { std::string url; std::size_t depth = 0; int attempts = 0; };
    std::string name, origin;
    Cache cache;
    Browser browser;
    std::shared_ptr<Session> session;
    CrawlOptions options;
    std::deque<Task> frontier;
    std::set<std::string> seen;
    std::size_t pages = 0, sequence = 0, errors = 0, seen_bytes = 0;
    std::string watch_url;
    std::size_t watch_seconds = 0;
    std::size_t revisit_at = 0;
    bool paused = true;
    Clock::time_point next = Clock::now(), robots_expiry{};
    std::vector<RobotRule> robots;
    bool robots_available = false;

    Impl(std::string n, const std::filesystem::path& directory, std::unique_ptr<NetworkClient> network)
        : name(std::move(n)), cache(directory), browser([] {
            BrowserConfig c; c.user_agent = "ProwseTkSpider/0.1";
            c.timeout_ms = 10000; c.max_response_bytes = 2u * 1024u * 1024u;
            c.max_redirects = 5; return c;
        }()) {
        if (!valid_name(name)) throw Error(ErrorCode::InvalidArgument, "invalid spider name");
        browser.set_network_client(std::make_unique<OriginNetwork>(
            network ? std::move(network) : make_socket_network_client(), &origin));
        session = browser.create_session();
        if (auto data = cache.get("state")) restore(*data);
        // Recover a sanitized snapshot without evaluating stale page scripts.
        if (sequence > 0) if (auto data = cache.get(page_key(sequence - 1))) {
            auto fields = unpack(*data);
            if (fields.size() == 5) session->load_html(fields[3], fields[0]);
        }
        paused = true;
        save();
    }
    std::string state() const {
        std::vector<std::string> fields{"2", origin, std::to_string(options.max_pages),
            std::to_string(options.max_depth), std::to_string(options.interval_ms),
            std::to_string(pages), std::to_string(sequence), std::to_string(errors)};
        std::vector<std::string> tasks;
        for (const auto& task : frontier)
            tasks.push_back(pack({task.url, std::to_string(task.depth), std::to_string(task.attempts)}));
        fields.push_back(pack(tasks));
        fields.push_back(pack({seen.begin(), seen.end()}));
        fields.push_back(watch_url);
        fields.push_back(std::to_string(watch_seconds));
        fields.push_back(std::to_string(revisit_at));
        return pack(fields);
    }
    void restore(const std::string& data) {
        const auto f = unpack(data);
        if (f.size() != 13 || f[0] != "2") throw Error(ErrorCode::StorageError, "unsupported state");
        origin = f[1];
        options.max_pages = integer(f[2], 10000); options.max_depth = integer(f[3], 32);
        options.interval_ms = static_cast<int>(integer(f[4], 3600000));
        pages = integer(f[5], 10000); sequence = integer(f[6], 1000000000); errors = integer(f[7], 1000000000);
        for (const auto& data_task : unpack(f[8])) {
            const auto t = unpack(data_task);
            if (t.size() != 3 || frontier.size() >= options.max_frontier)
                throw Error(ErrorCode::StorageError, "invalid frontier");
            frontier.push_back({t[0], integer(t[1], 32), static_cast<int>(integer(t[2], 3))});
        }
        seen_bytes = 0;
        for (const auto& url : unpack(f[9])) { seen.insert(url); seen_bytes += url.size(); }
        watch_url = f[10]; watch_seconds = integer(f[11], 86400 * 30);
        revisit_at = integer(f[12], 10000000000ULL);
        if (seen.size() > options.max_frontier || seen_bytes > 1024u * 1024u)
            throw Error(ErrorCode::StorageError, "invalid seen set");
    }
    void save() { cache.write({{"state", state()}}); }
    std::string allowed(std::string_view url) const {
        auto normalized = canonical(url);
        if (parse_url(normalized).origin() != origin)
            throw Error(ErrorCode::SecurityViolation, "outside spider origin");
        return normalized;
    }
    void enqueue(std::string_view url, std::size_t depth) {
        const auto normalized = allowed(url);
        if (depth > options.max_depth || seen.contains(normalized)) return;
        if (seen.size() >= options.max_frontier || seen_bytes + normalized.size() > 1024u * 1024u)
            throw Error(ErrorCode::ResourceLimit, "frontier full");
        seen.insert(normalized); seen_bytes += normalized.size(); frontier.push_back({normalized, depth, 0});
    }
    Reply status() const {
        return {true, "{\"name\":" + json_quote(name) + ",\"state\":" +
            json_quote(paused ? "paused" : (frontier.empty() ? "idle" : "running")) +
            ",\"url\":" + json_quote(Redactor{}.redact_url(session->current_url())) +
            ",\"pages\":" + std::to_string(pages) + ",\"pending\":" + std::to_string(frontier.size()) +
            ",\"errors\":" + std::to_string(errors) + ",\"max_pages\":" + std::to_string(options.max_pages) +
            ",\"watch_seconds\":" + std::to_string(watch_seconds) +
            ",\"next_crawl_at\":" + std::to_string(revisit_at) + "}"};
    }
    void snapshot(int status_code, std::vector<std::pair<std::string, std::string>> puts = {}) {
        const auto doc = session->document();
        if (!doc) { puts.emplace_back("state", state()); cache.write(puts); return; }
        const auto now = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        const auto key = page_key(sequence++);
        auto html = sanitized_html(*doc);
        if (html.size() > 256u * 1024u) html.clear();
        const auto value = pack({Redactor{}.redact_url(session->current_url()), doc->title(),
            std::to_string(status_code), html, std::to_string(now)});
        puts.emplace_back(key, value); puts.emplace_back("state", state());
        cache.write(puts);
    }
    std::size_t record_count() const {
        std::size_t count = 0;
        std::string after;
        for (;;) {
            const auto rows = cache.query("record/", 100, after);
            if (rows.empty()) return count;
            count += rows.size(); after = rows.back().first;
            if (count > 1000) return count;
        }
    }
    bool robots_allow(const std::string& url) {
        if (Clock::now() >= robots_expiry) {
            robots_available = false;
            HttpRequest request; request.url = origin + "/robots.txt"; request.max_response_bytes = 65536;
            const auto response = session->request(request);
            robots_available = response.ok() || response.status == 404 || response.status == 410;
            robots = response.ok() ? parse_robots(response.body) : std::vector<RobotRule>{};
            robots_expiry = Clock::now() + (robots_available ? std::chrono::hours(1) : std::chrono::hours(0));
        }
        if (!robots_available) throw Error(ErrorCode::NetworkError, "robots unavailable");
        const auto parsed = parse_url(url);
        const auto path = parsed.path + (parsed.has_query ? "?" + parsed.query : "");
        bool allow = true;
        bool matched = false;
        std::size_t best = 0;
        for (const auto& rule : robots) if (robot_match(rule.path, path)) {
            const auto length = static_cast<std::size_t>(std::count_if(rule.path.begin(), rule.path.end(),
                [](char c) { return c != '*' && c != '$'; }));
            if (!matched || length > best || (length == best && rule.allow)) {
                matched = true; best = length; allow = rule.allow;
            }
        }
        return allow;
    }
    Reply driver(const std::vector<std::string>& args) {
        if (!LuaRuntime::available()) return failure("Unsupported");
        if (args.size() < 2 || args.size() > 66 || args.size() % 2 != 0) return failure("InvalidArgument");
        // Script bytes are supplied by the controller; no daemon-side arbitrary
        // path lookup, shell evaluation or accidental use of client-relative paths.
        if (args[1].size() > 256u * 1024u) return failure("ResourceLimit");
        LuaRuntime lua; lua.bind_browser(&browser); lua.bind_session(session);
        std::string bootstrap = "__spider_records={}";
        for (const auto& [key, value] : cache.query("record/", 128))
            bootstrap += "\n__spider_records[" + lua_string(key.substr(7)) + "]=" + lua_string(value);
        auto result = lua.run(bootstrap);
        if (!result.ok) return failure("LuaError");
        result = lua.run(spider_lua_source, "lspider");
        if (!result.ok) return failure("LuaError");
        // Drivers are owner-trusted automation, not a security sandbox. Suppress
        // output and cap normal Lua instruction loops. Daemon watchdog/process
        // limits cover hangs and excessive allocations in the worker.
        result = lua.run(R"LUA(
            local instructions = 0
            debug.sethook(function()
                instructions = instructions + 10000
                if instructions > 5000000 then error('driver instruction limit') end
            end, '', 10000)
            print = function() end
            io = nil; os = nil; debug = nil; dofile = nil; loadfile = nil
            package.loadlib = nil; package.path = ''; package.cpath = ''
        )LUA");
        if (!result.ok || !lua.run(args[1], "spider-driver").ok) return failure("LuaError");
        std::vector<LuaArgument> arguments;
        for (std::size_t i = 2; i < args.size(); i += 2) arguments.push_back({args[i], "string", args[i + 1]});
        std::string exit;
        if (!lua.call_function("main", arguments, &exit).ok || exit != "0") return failure("LuaError");
        std::string operations;
        if (!lua.call_function("__spider_finish", {}, &operations).ok) return failure("LuaError");
        auto old_frontier = frontier;
        auto old_seen = seen;
        std::vector<std::pair<std::string, std::string>> puts;
        try {
            for (const auto& item : unpack(operations)) {
                const auto operation = unpack(item);
                if (operation.size() != 3) throw Error(ErrorCode::LuaError, "invalid operation");
                if (operation[0] == "enqueue") enqueue(operation[1], integer(operation[2], 32));
                else if (operation[0] == "put") {
                    if (operation[1].empty() || operation[1].size() > 128 || operation[2].size() > 65536)
                        throw Error(ErrorCode::ResourceLimit, "record too large");
                    puts.emplace_back("record/" + operation[1], sensitive(operation[1]) ? "[REDACTED]" : operation[2]);
                } else throw Error(ErrorCode::LuaError, "invalid operation");
            }
            std::set<std::string> new_keys;
            for (const auto& [key, value] : puts) {
                (void)value;
                if (!cache.get(key)) new_keys.insert(key);
            }
            if (record_count() + new_keys.size() > 1000)
                throw Error(ErrorCode::ResourceLimit, "too many records");
            snapshot(0, std::move(puts));
        } catch (...) { frontier = std::move(old_frontier); seen = std::move(old_seen); throw; }
        return status();
    }
    Reply command(const std::vector<std::string>& args) {
        if (args.empty()) return failure("InvalidArgument");
        const auto& cmd = args[0];
        if (cmd == "status" && args.size() == 1) return status();
        if ((cmd == "pause" || cmd == "resume") && args.size() == 1) {
            paused = cmd == "pause"; next = Clock::now(); save(); return status();
        }
        if (cmd == "unwatch" && args.size() == 1) {
            watch_seconds = 0; watch_url.clear(); revisit_at = 0; save(); return status();
        }
        if (cmd == "watch" && args.size() >= 3 && args.size() <= 6) {
            const auto period = integer(args[2], 86400 * 30);
            if (period == 0) return failure("InvalidArgument");
            std::vector<std::string> crawl{"crawl", args[1]};
            crawl.insert(crawl.end(), args.begin() + 3, args.end());
            const auto result = command(crawl);
            if (!result.ok) return result;
            watch_url = allowed(args[1]); watch_seconds = period; revisit_at = 0;
            save(); return status();
        }
        if (cmd == "create" || cmd == "crawl") {
            if (args.size() < 2 || args.size() > 5) return failure("InvalidArgument");
            const auto seed = canonical(args[1]);
            if (!origin.empty() && origin != parse_url(seed).origin()) return failure("SecurityViolation");
            auto chosen = options;
            if (args.size() > 2) chosen.max_pages = integer(args[2], 10000);
            if (args.size() > 3) chosen.max_depth = integer(args[3], 32);
            if (args.size() > 4) chosen.interval_ms = static_cast<int>(integer(args[4], 3600000));
            if (chosen.max_pages == 0) return failure("InvalidArgument");
            origin = parse_url(seed).origin(); options = chosen;
            watch_url.clear(); watch_seconds = 0; revisit_at = 0;
            frontier.clear(); seen.clear(); seen_bytes = 0; pages = 0; errors = 0;
            enqueue(seed, 0); paused = cmd == "create"; next = Clock::now(); save();
            return status();
        }
        if (cmd == "enqueue" && (args.size() == 2 || args.size() == 3)) {
            enqueue(args[1], args.size() == 3 ? integer(args[2], 32) : 0); save(); return status();
        }
        if (cmd == "load-html" && (args.size() == 2 || args.size() == 3)) {
            const auto url = allowed(args.size() == 3 ? args[2] : origin + "/");
            session->load_html(args[1], url); snapshot(0); return status();
        }
        if (cmd == "navigate" && args.size() == 2) {
            session->navigate(allowed(args[1])); snapshot(0); return status();
        }
        if (cmd == "driver") return driver(args);
        if ((cmd == "click" && args.size() >= 2 && args.size() <= 3) ||
            (cmd == "type" && args.size() >= 3 && args.size() <= 4)) {
            if (!session->document()) return failure("NotFound");
            const auto nodes = session->document()->query_selector_all(args[1]);
            const auto index = args.size() == (cmd == "click" ? 3u : 4u) ? integer(args.back(), 250000) : 1;
            if (index == 0 || index > nodes.size()) return failure("NotFound");
            const bool ok = cmd == "click" ? session->click_element(nodes[index - 1]) :
                session->type_element(nodes[index - 1], args[2]);
            if (!ok) return failure("InvalidArgument");
            snapshot(0); return status();
        }
        if ((cmd == "query" || cmd == "xpath") && args.size() == 2) {
            if (!session->document()) return failure("NotFound");
            const auto safe = parse_html(sanitized_html(*session->document()));
            std::vector<std::string> results;
            if (cmd == "query") for (const auto& node : safe->query_selector_all(args[1])) {
                if (results.size() == 100) break;
                results.push_back(node->outer_html());
            }
            else {
                const auto value = evaluate_xpath(*safe, args[1]);
                if (value.type == XPathValueType::NodeSet) results = value.string_values;
                else results.push_back(xpath_string_value(*safe, args[1]));
            }
            std::string out = "[";
            for (std::size_t i = 0; i < std::min(results.size(), std::size_t{100}); ++i) {
                if (i) out += ',';
                out += json_quote(results[i]);
                if (out.size() > max_message_bytes / 2) return failure("ResourceLimit");
            }
            return {true, out + "]"};
        }
        if (cmd == "cache" && args.size() >= 2) {
            if (args[1] == "select" && (args.size() == 3 || args.size() == 4)) {
                const auto limit = args.size() == 4 ? integer(args[3], 100) : 100;
                std::string out = "[";
                for (const auto& [key, value] : cache.query("page/", limit)) {
                    const auto fields = unpack(value);
                    if (fields.size() != 5 || fields[3].empty()) continue;
                    const auto doc = parse_html(fields[3]);
                    std::string matches = "[";
                    std::size_t count = 0;
                    for (const auto& node : doc->query_selector_all(args[2])) {
                        if (++count > 100) break;
                        if (matches.size() > 1) matches += ',';
                        matches += json_quote(node->outer_html());
                        if (matches.size() > 512u * 1024u) break;
                    }
                    const auto item = "{\"key\":" + json_quote(key) + ",\"url\":" + json_quote(fields[0]) +
                        ",\"matches\":" + matches + "]}";
                    if (out.size() + item.size() > max_message_bytes / 2) break;
                    if (out.size() > 1) out += ',';
                    out += item;
                }
                return {true, out + "]"};
            }
            if (args[1] == "put" && args.size() == 4) {
                if (args[2].empty() || args[2].size() > 128 || args[3].size() > 65536)
                    return failure("ResourceLimit");
                if (!cache.get("record/" + args[2]) && record_count() >= 1000)
                    return failure("ResourceLimit");
                cache.write({{"record/" + args[2], sensitive(args[2]) ? "[REDACTED]" : args[3]}});
                return {true, "{}"};
            }
            if (args[1] == "get" && args.size() == 3) {
                if (!args[2].starts_with("page/") && !args[2].starts_with("record/")) return failure("InvalidArgument");
                const auto value = cache.get(args[2]);
                if (!value) return failure("NotFound");
                return {true, args[2].starts_with("page/") ? page_json(*value) : json_quote(*value)};
            }
            if (args[1] == "query" && args.size() >= 2 && args.size() <= 5) {
                const auto prefix = args.size() > 2 ? args[2] : "page/";
                if (!prefix.starts_with("page/") && !prefix.starts_with("record/")) return failure("InvalidArgument");
                const auto limit = args.size() > 3 ? integer(args[3], 100) : 100;
                std::string out = "[";
                for (const auto& [key, value] : cache.query(prefix, limit, args.size() > 4 ? args[4] : "")) {
                    const auto item = "{\"key\":" + json_quote(key) + ",\"value\":" +
                        (key.starts_with("page/") ? page_json(value) : json_quote(value)) + "}";
                    if (out.size() + item.size() > max_message_bytes / 2) break;
                    if (out.size() > 1) out += ',';
                    out += item;
                }
                return {true, out + "]"};
            }
        }
        return failure("InvalidArgument");
    }
    bool tick() {
        if (paused || Clock::now() < next) return false;
        if (frontier.empty() || pages >= options.max_pages) {
            if (!watch_seconds) {
                if (pages >= options.max_pages) paused = true;
                return false;
            }
            if (revisit_at == 0) {
                frontier.clear(); revisit_at = epoch_seconds() + watch_seconds; save(); return false;
            }
            if (epoch_seconds() < revisit_at) return false;
            frontier.clear(); seen.clear(); seen_bytes = 0; pages = 0; errors = 0; revisit_at = 0;
            enqueue(watch_url, 0); save();
        }
        auto task = frontier.front();
        const auto before = state();
        int retry_delay = options.interval_ms;
        try {
            if (!robots_allow(task.url)) { frontier.pop_front(); save(); return true; }
            HttpRequest request; request.url = task.url;
            const auto response = session->request(request);
            if (response.status == 429 || response.status >= 500) {
                const auto delay = response.header("Retry-After");
                if (!delay.empty()) try { retry_delay = static_cast<int>(integer(delay, 3600)) * 1000; } catch (...) {}
                throw Error(ErrorCode::NetworkError, "retryable response");
            }
            if (!response.ok()) { ++errors; frontier.pop_front(); save(); return true; }
            session->load_html(response.body, response.final_url.empty() ? task.url : allowed(response.final_url));
            // Keep the task leased in the durable state until links and the page
            // are committed together. A crash before commit replays this GET.
            if (task.depth < options.max_depth) for (const auto& link : session->document()->links()) {
                try { enqueue(resolve_url(session->document()->base_url(), link->attribute("href")), task.depth + 1); }
                catch (const Error& e) { if (e.code() == ErrorCode::ResourceLimit) break; }
            }
            frontier.pop_front(); ++pages; snapshot(response.status);
        } catch (const Error& e) {
            frontier.clear(); seen.clear(); restore(before);
            ++errors;
            if (e.code() == ErrorCode::SecurityViolation || ++task.attempts > options.max_retries) frontier.pop_front();
            else { frontier.front().attempts = task.attempts; retry_delay = std::max(retry_delay, 1000 * (1 << task.attempts)); }
            save();
        }
        next = Clock::now() + std::chrono::milliseconds(retry_delay);
        return true;
    }
};
Engine::Engine(std::string name, const std::filesystem::path& directory, std::unique_ptr<NetworkClient> network)
    : impl_(std::make_unique<Impl>(std::move(name), directory, std::move(network))) {}
Engine::~Engine() = default;
Reply Engine::command(const std::vector<std::string>& args) {
    const auto before = impl_->state();
    try { return impl_->command(args); }
    catch (const Error& error) {
        impl_->frontier.clear(); impl_->seen.clear(); impl_->restore(before);
        return failure(to_string(error.code()));
    }
    catch (...) {
        impl_->frontier.clear(); impl_->seen.clear(); impl_->restore(before);
        impl_->paused = true; return failure("StorageError");
    }
}
bool Engine::tick() {
    const auto before = impl_->state();
    try { return impl_->tick(); }
    catch (...) {
        impl_->frontier.clear(); impl_->seen.clear(); impl_->restore(before);
        impl_->paused = true; return false;
    }
}
bool Engine::running() const noexcept { return !impl_->paused; }
}
