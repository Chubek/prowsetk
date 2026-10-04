#include "prowsetk/plugins/opencode_marionette.hpp"
#include "prowsetk/error.hpp"
#include <charconv>
#include <fstream>
#include <set>

namespace prowsetk::plugins::opencode_marionette {
namespace {
using J = opencode_bridge::JsonValue;
[[noreturn]] void invalid() { throw Error(ErrorCode::ParseError, "opencode-marionette: invalid decisions JSON"); }
const J* field(const J& value, std::string_view name) {
    for (const auto& entry : value.fields) if (entry.first == name) return entry.second.get();
    return nullptr;
}
void keys(const J& value, const std::set<std::string>& allowed) {
    if (value.type != J::Type::Object) invalid();
    std::set<std::string> seen;
    for (const auto& entry : value.fields) {
        if (!allowed.contains(entry.first) || !seen.insert(entry.first).second) invalid();
    }
}
std::string string(const J& value, std::string_view name, bool required = true) {
    const auto* item = field(value, name);
    if (!item && !required) return {};
    if (!item || item->type != J::Type::String || item->str.empty() || item->str.size() > 4096 ||
        item->str.find('\0') != std::string::npos) invalid();
    return item->str;
}
unsigned number(const J& value, std::string_view name, unsigned fallback, unsigned cap, bool zero = false) {
    const auto* item = field(value, name);
    if (!item) return fallback;
    if (item->type != J::Type::Number) invalid();
    unsigned out = 0;
    const auto parsed = std::from_chars(item->str.data(), item->str.data() + item->str.size(), out);
    if (parsed.ec != std::errc{} || parsed.ptr != item->str.data() + item->str.size() ||
        (!zero && out == 0) || out > cap) invalid();
    return out;
}
J parse(std::string_view json) {
    if (json.size() > 65536) throw Error(ErrorCode::ResourceLimit, "opencode-marionette: JSON byte limit exceeded");
    // The shared bridge parser is permissive about literal control bytes in
    // strings. Decisions must follow strict JSON string grammar.
    bool quoted = false, escaped = false;
    for (unsigned char c : json) {
        if (quoted && c < 0x20) invalid();
        if (escaped) { escaped = false; continue; }
        if (quoted && c == '\\') { escaped = true; continue; }
        if (c == '"') quoted = !quoted;
    }
    J root;
    if (!opencode_bridge::json_parse(json, root)) invalid();
    return root;
}
}
Decisions parse_decisions(std::string_view json) {
    auto root = parse(json);
    keys(root, {"version", "goal", "max_steps", "max_page_requests", "max_get_probes", "actions"});
    if (!field(root, "version") || number(root, "version", 0, 1) != 1) invalid();
    Decisions out;
    out.goal = string(root, "goal");
    out.max_steps = number(root, "max_steps", 16, 64);
    out.max_page_requests = number(root, "max_page_requests", 128, 1024);
    out.max_get_probes = number(root, "max_get_probes", 32, 128, true);
    const auto* actions = field(root, "actions");
    if (!actions || actions->type != J::Type::Array || actions->items.size() > 128) invalid();
    std::set<std::string> ids;
    for (const auto& item : actions->items) {
        keys(item, {"id", "kind", "selector", "value", "url", "max_uses"});
        Action action;
        action.id = string(item, "id");
        if (action.id == "stop" || action.id.size() > 64 || !ids.insert(action.id).second) invalid();
        for (unsigned char c : action.id) {
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '-')) invalid();
        }
        action.kind = string(item, "kind");
        action.max_uses = number(item, "max_uses", 1, 64);
        if (action.kind == "navigate") {
            action.url = string(item, "url");
            if (field(item, "selector") || field(item, "value")) invalid();
        } else if (action.kind == "click" || action.kind == "type") {
            action.selector = string(item, "selector");
            if (field(item, "url")) invalid();
            if (action.kind == "type") action.value = string(item, "value");
            else if (field(item, "value")) invalid();
        } else invalid();
        out.actions.push_back(std::move(action));
    }
    return out;
}
Decisions load_decisions(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw Error(ErrorCode::IoError, "opencode-marionette: cannot read decisions file");
    std::string json(65537, '\0');
    input.read(json.data(), static_cast<std::streamsize>(json.size()));
    const auto count = input.gcount();
    if (input.bad()) throw Error(ErrorCode::IoError, "opencode-marionette: decisions read failed");
    json.resize(static_cast<std::size_t>(count));
    return parse_decisions(json);
}
std::string parse_choice(std::string_view json, const Decisions& decisions) {
    auto root = parse(json);
    keys(root, {"action"});
    auto action = string(root, "action");
    if (action == "stop") return action;
    for (const auto& allowed : decisions.actions) if (allowed.id == action) return action;
    throw Error(ErrorCode::SecurityViolation, "opencode-marionette: action is not allowed");
}
}
