// OpenCode HTTP/SSE client. All requests go through the borrowed host
// NetworkClient; no sockets are opened here. Redirects are rejected, input
// and response sizes are bounded, and errors never echo secrets.

#include "opencode_bridge.hpp"

#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string_view>
#include <thread>

#include "prowsetk/error.hpp"
#include "prowsetk/url.hpp"

namespace prowsetk::plugins::opencode_bridge {
namespace {

[[noreturn]] void invalid(std::string_view what) {
    throw Error(ErrorCode::InvalidArgument, "opencode-bridge: invalid " + std::string(what));
}

bool header_safe(std::string_view value) {
    if (value.size() > 4096) return false;
    for (unsigned char c : value) {
        if (c < 0x20 || c == 0x7f) return false;
    }
    return true;
}

std::string normalize_base(std::string_view base) {
    std::string out(base);
    while (out.size() > 1 && out.back() == '/') out.pop_back();
    return out;
}

void check_base_url(std::string_view base, bool allow_remote_http) {
    if (base.empty() || !header_safe(base) || base.find(' ') != std::string_view::npos) {
        invalid("base URL");
    }
    Url url;
    try {
        url = parse_url(base);
    } catch (...) {
        invalid("base URL");
    }
    // Local bridge traffic only: loopback HTTP or any HTTPS endpoint, unless
    // the host explicitly opts into remote plain HTTP.
    if ((url.scheme != "http" && url.scheme != "https") || !url.has_host()) invalid("base URL");
    if (url.has_query || url.has_fragment || !url.userinfo.empty()) invalid("base URL");
    if (url.scheme == "http" && !allow_remote_http) {
        std::string host = url.host;
        // Strip brackets from IPv6 literals for the loopback comparison.
        if (host.size() > 2 && host.front() == '[' && host.back() == ']') {
            host = host.substr(1, host.size() - 2);
        }
        std::string lower;
        lower.reserve(host.size());
        for (unsigned char c : host) lower.push_back(static_cast<char>(std::tolower(c)));
        const bool loopback = lower == "localhost" || lower == "127.0.0.1" || lower == "::1";
        if (!loopback) invalid("base URL");
    }
}

std::string base64_encode(std::string_view input) {
    static constexpr char kTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    unsigned value = 0;
    int bits = -6;
    for (unsigned char c : input) {
        value = (value << 8) | c;
        bits += 8;
        while (bits >= 0) {
            out.push_back(kTable[(value >> bits) & 0x3f]);
            bits -= 6;
        }
    }
    if (bits > -6) out.push_back(kTable[((value << 8) >> (bits + 8)) & 0x3f]);
    while (out.size() % 4) out.push_back('=');
    return out;
}

void check_session_id(std::string_view id) {
    if (id.empty() || id.size() > 256 || !header_safe(id) ||
        id.find_first_of(" \t\r\n/?#") != std::string_view::npos) {
        invalid("session id");
    }
    // The id is interpolated into request paths; `.`/`..` could escape the
    // /session/ scope on servers that normalize dot segments.
    if (id == "." || id == "..") invalid("session id");
}

std::string percent_encode_impl(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (unsigned char c : value) {
        const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                                c == '.' || c == '~';
        if (unreserved) {
            out.push_back(static_cast<char>(c));
        } else {
            char buf[4];
            snprintf(buf, sizeof(buf), "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

// Decodes a JSON string literal starting at `*at` (which must point at the
// opening quote). Advances `at` past the closing quote. Surrogate pairs are
// combined; lone surrogates become U+FFFD rather than invalid UTF-8.
bool decode_json_string(std::string_view body, std::size_t& at, std::string& out) {
    if (at >= body.size() || body[at] != '"') return false;
    auto append_codepoint = [&](unsigned code) {
        if (code < 0x80) {
            out.push_back(static_cast<char>(code));
        } else if (code < 0x800) {
            out.push_back(static_cast<char>(0xc0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
        } else if (code < 0x10000) {
            out.push_back(static_cast<char>(0xe0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
        } else {
            out.push_back(static_cast<char>(0xf0 | (code >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
        }
    };
    auto parse_hex4 = [&](unsigned& code) {
        if (at + 4 > body.size()) return false;
        code = 0;
        for (int i = 0; i < 4; ++i) {
            const char h = body[at++];
            code <<= 4;
            if (h >= '0' && h <= '9') code |= static_cast<unsigned>(h - '0');
            else if (h >= 'a' && h <= 'f') code |= static_cast<unsigned>(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') code |= static_cast<unsigned>(h - 'A' + 10);
            else return false;
        }
        return true;
    };
    ++at;
    out.clear();
    while (at < body.size()) {
        const char c = body[at++];
        if (c == '"') return true;
        if (c != '\\') {
            out.push_back(c);
            continue;
        }
        if (at >= body.size()) return false;
        const char e = body[at++];
        switch (e) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                unsigned code = 0;
                if (!parse_hex4(code)) return false;
                if (code >= 0xd800 && code <= 0xdbff) {
                    // High surrogate: require a \uDC00-\uDFFF tail.
                    if (at + 6 > body.size() || body[at] != '\\' || body[at + 1] != 'u') {
                        append_codepoint(0xfffd);
                        break;
                    }
                    at += 2;
                    unsigned low = 0;
                    if (!parse_hex4(low) || low < 0xdc00 || low > 0xdfff) return false;
                    append_codepoint(0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00));
                } else if (code >= 0xdc00 && code <= 0xdfff) {
                    append_codepoint(0xfffd);
                } else {
                    append_codepoint(code);
                }
                break;
            }
            default: return false;
        }
        if (out.size() > kMaxResponseBytesHardCap) return false;
    }
    return false;
}

}  // namespace

namespace {

// Forward declarations for the JSON document parser.
bool parse_json_value(std::string_view body, std::size_t& at, JsonValue& out, int depth);

void skip_json_ws(std::string_view body, std::size_t& at) {
    while (at < body.size() && (body[at] == ' ' || body[at] == '\t' ||
                                body[at] == '\r' || body[at] == '\n')) {
        ++at;
    }
}

bool parse_json_literal(std::string_view body, std::size_t& at, JsonValue& out);

bool parse_json_value(std::string_view body, std::size_t& at, JsonValue& out, int depth) {
    if (depth > 32) return false;
    skip_json_ws(body, at);
    if (at >= body.size()) return false;
    const char c = body[at];
    if (c == '"') {
        std::string text;
        if (!decode_json_string(body, at, text)) return false;
        out = JsonValue{};
        out.type = JsonValue::Type::String;
        out.str = std::move(text);
        return true;
    }
    if (c == '{') {
        ++at;
        out = JsonValue{};
        out.type = JsonValue::Type::Object;
        skip_json_ws(body, at);
        if (at < body.size() && body[at] == '}') {
            ++at;
            return true;
        }
        for (;;) {
            skip_json_ws(body, at);
            if (at >= body.size() || body[at] != '"') return false;
            std::string key;
            if (!decode_json_string(body, at, key)) return false;
            skip_json_ws(body, at);
            if (at >= body.size() || body[at] != ':') return false;
            ++at;
            JsonValue value;
            if (!parse_json_value(body, at, value, depth + 1)) return false;
            out.fields.emplace_back(std::move(key),
                                    std::make_unique<JsonValue>(std::move(value)));
            skip_json_ws(body, at);
            if (at >= body.size()) return false;
            if (body[at] == ',') {
                ++at;
                continue;
            }
            if (body[at] == '}') {
                ++at;
                return true;
            }
            return false;
        }
    }
    if (c == '[') {
        ++at;
        out = JsonValue{};
        out.type = JsonValue::Type::Array;
        skip_json_ws(body, at);
        if (at < body.size() && body[at] == ']') {
            ++at;
            return true;
        }
        for (;;) {
            JsonValue value;
            if (!parse_json_value(body, at, value, depth + 1)) return false;
            out.items.push_back(std::move(value));
            skip_json_ws(body, at);
            if (at >= body.size()) return false;
            if (body[at] == ',') {
                ++at;
                continue;
            }
            if (body[at] == ']') {
                ++at;
                return true;
            }
            return false;
        }
    }
    return parse_json_literal(body, at, out);
}

bool is_json_boundary(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ',' ||
           c == ']' || c == '}' || c == ':';
}

bool parse_json_literal(std::string_view body, std::size_t& at, JsonValue& out) {
    for (const char* word : {"true", "false", "null"}) {
        const std::size_t len = std::strlen(word);
        if (body.compare(at, len, word) == 0 &&
            (at + len >= body.size() || is_json_boundary(body[at + len]))) {
            at += len;
            out = JsonValue{};
            out.type = word[0] == 'n' ? JsonValue::Type::Null : JsonValue::Type::Bool;
            out.boolean = word[0] == 't';
            return true;
        }
    }
    // Number: -?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?
    const std::size_t start = at;
    if (at < body.size() && body[at] == '-') ++at;
    if (at >= body.size()) return false;
    if (body[at] == '0') {
        ++at;
    } else if (body[at] >= '1' && body[at] <= '9') {
        while (at < body.size() && body[at] >= '0' && body[at] <= '9') ++at;
    } else {
        return false;
    }
    if (at < body.size() && body[at] == '.') {
        ++at;
        if (at >= body.size() || body[at] < '0' || body[at] > '9') return false;
        while (at < body.size() && body[at] >= '0' && body[at] <= '9') ++at;
    }
    if (at < body.size() && (body[at] == 'e' || body[at] == 'E')) {
        ++at;
        if (at < body.size() && (body[at] == '+' || body[at] == '-')) ++at;
        if (at >= body.size() || body[at] < '0' || body[at] > '9') return false;
        while (at < body.size() && body[at] >= '0' && body[at] <= '9') ++at;
    }
    if (at < body.size() && !is_json_boundary(body[at])) return false;
    out = JsonValue{};
    out.type = JsonValue::Type::Number;
    out.str.assign(body.substr(start, at - start));
    return true;
}

// Document-order first match: for each field in order, a string-valued key
// match wins before descending into that field's value.
bool find_first_string(const JsonValue& value, std::string_view key, std::string& out) {
    if (value.type == JsonValue::Type::Array) {
        for (const auto& item : value.items) {
            if (find_first_string(item, key, out)) return true;
        }
        return false;
    }
    if (value.type != JsonValue::Type::Object) return false;
    for (const auto& [name, child] : value.fields) {
        if (child == nullptr) return false;
        if (name == key && child->type == JsonValue::Type::String) {
            out = child->str;
            return true;
        }
        if (find_first_string(*child, key, out)) return true;
    }
    return false;
}

}  // namespace

BridgeConfig config_from_environment() {
    BridgeConfig config;
    if (const char* base = std::getenv("OPENCODE_BASE_URL")) {
        if (*base) config.base_url = base;
    }
    if (const char* user = std::getenv("OPENCODE_SERVER_USERNAME")) config.username = user;
    if (const char* pass = std::getenv("OPENCODE_SERVER_PASSWORD")) config.password = pass;
    return config;
}

void validate_config(const BridgeConfig& config) {
    check_base_url(config.base_url, config.allow_remote_http);
    if (!header_safe(config.username) || !header_safe(config.password)) invalid("credentials");
    if (config.api_prefix != "" && config.api_prefix != "/api") invalid("API prefix");
    if (config.timeout_ms <= 0 || config.timeout_ms > 300000) invalid("timeout");
    if (config.prompt_wait_ms < 0 || config.prompt_wait_ms > 600000) invalid("prompt wait");
    if (config.max_input_bytes == 0 || config.max_input_bytes > kMaxInputBytesHardCap) {
        invalid("input budget");
    }
    if (config.max_response_bytes == 0 || config.max_response_bytes > kMaxResponseBytesHardCap) {
        invalid("response budget");
    }
    if (config.max_requests == 0 || config.max_requests > 1024) invalid("request budget");
}

std::string json_escape(std::string_view text) {
    std::string out = "\"";
    for (unsigned char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    out.push_back('"');
    return out;
}

bool json_extract_string(std::string_view body, std::string_view key, std::string& out) {
    // Document-order first match via the real parser: keys inside string
    // literals never match, and non-string values are skipped for later
    // string-valued occurrences.
    JsonValue root;
    if (!json_parse(body, root)) return false;
    return find_first_string(root, key, out);
}

bool json_parse(std::string_view body, JsonValue& out) {
    if (body.size() > kMaxResponseBytesHardCap) return false;
    std::size_t at = 0;
    JsonValue root;
    if (!parse_json_value(body, at, root, 0)) return false;
    skip_json_ws(body, at);
    if (at != body.size()) return false;
    out = std::move(root);
    return true;
}

bool json_find_string_at_path(const JsonValue& root,
                              std::initializer_list<std::string_view> path, std::string& out) {
    const JsonValue* node = &root;
    for (std::string_view segment : path) {
        if (node->type != JsonValue::Type::Object) return false;
        const JsonValue* next = nullptr;
        for (const auto& [name, child] : node->fields) {
            if (name == segment && child != nullptr) {
                next = child.get();
                break;
            }
        }
        if (next == nullptr) return false;
        node = next;
    }
    if (node->type != JsonValue::Type::String) return false;
    out = node->str;
    return true;
}

namespace {

const JsonValue* object_field(const JsonValue& node, std::string_view name) {
    if (node.type != JsonValue::Type::Object) return nullptr;
    for (const auto& [key, child] : node.fields) {
        if (key == name && child != nullptr) return child.get();
    }
    return nullptr;
}

bool field_string(const JsonValue& node, std::string_view name, std::string& out) {
    const JsonValue* field = object_field(node, name);
    if (field == nullptr || field->type != JsonValue::Type::String) return false;
    out = field->str;
    return true;
}

}  // namespace

std::vector<AssistantMessage> parse_assistant_messages(const JsonValue& root) {
    const JsonValue* list = &root;
    if (root.type == JsonValue::Type::Object) {
        list = object_field(root, "data");
        if (list == nullptr || list->type != JsonValue::Type::Array) return {};
    } else if (root.type != JsonValue::Type::Array) {
        return {};
    }
    std::vector<AssistantMessage> messages;
    for (const auto& item : list->items) {
        if (item.type != JsonValue::Type::Object) continue;
        std::string type;
        if (!field_string(item, "type", type)) continue;
        AssistantMessage message;
        if (type == "idle") {
            message.idle = true;
            std::string outcome;
            message.succeeded = field_string(item, "outcome", outcome) && outcome == "succeeded";
            if (!field_string(item, "id", message.id)) continue;
            messages.push_back(std::move(message));
            continue;
        }
        if (type != "assistant") continue;
        if (!field_string(item, "id", message.id)) continue;
        const JsonValue* time = object_field(item, "time");
        message.completed = time != nullptr && object_field(*time, "completed") != nullptr;
        const auto* error = object_field(item, "error");
        std::string finish;
        message.failed = (error != nullptr && error->type != JsonValue::Type::Null) ||
            (field_string(item, "finish", finish) && finish == "error");
        const JsonValue* content = object_field(item, "content");
        if (content != nullptr && content->type == JsonValue::Type::Array) {
            for (const auto& part : content->items) {
                if (part.type != JsonValue::Type::Object) continue;
                std::string part_type, text;
                if (field_string(part, "type", part_type) && part_type == "text" &&
                    field_string(part, "text", text)) {
                    if (!message.text.empty()) message.text.push_back('\n');
                    message.text += text;
                }
            }
        }
        messages.push_back(std::move(message));
    }
    return messages;
}

std::vector<std::string> parse_sse_events(std::string_view body) {
    // Proper text/event-stream framing: consecutive `data:` lines belong to one
    // event (joined with "\n") and a blank line dispatches it. `[DONE]` events
    // are dropped. Bounded at 1024 dispatched events.
    std::vector<std::string> events;
    std::string pending;
    bool has_pending = false;
    auto dispatch = [&] {
        if (!has_pending) return;
        has_pending = false;
        if (pending != "[DONE]") events.push_back(std::move(pending));
        pending.clear();
    };
    std::size_t at = 0;
    while (at <= body.size() && events.size() < 1024) {
        auto end = body.find('\n', at);
        std::string_view line = (end == std::string_view::npos)
            ? body.substr(at)
            : body.substr(at, end - at);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.empty() || line[0] == ':') {
            if (line.empty()) dispatch();
        } else if (line.starts_with("data:")) {
            std::string_view payload = line.substr(5);
            if (!payload.empty() && payload.front() == ' ') payload.remove_prefix(1);
            if (has_pending) pending.push_back('\n');
            pending.append(payload.data(), payload.size());
            has_pending = true;
        } else if (line.starts_with("event:") || line.starts_with("id:") ||
                   line.starts_with("retry:")) {
            // Framing fields: intentionally ignored.
        } else {
            // Unknown field: per SSE, ignored (forward-compatible).
        }
        if (end == std::string_view::npos) break;
        at = end + 1;
    }
    dispatch();
    return events;
}

std::string percent_encode(std::string_view value) { return percent_encode_impl(value); }

OpenCodeClient::OpenCodeClient(NetworkClient& network, BridgeConfig config)
    : network_(network), config_(std::move(config)) {
    validate_config(config_);
}

HttpResponse OpenCodeClient::send(std::string method, std::string url, std::string body) {
    if (request_count_ >= config_.max_requests) {
        throw Error(ErrorCode::ResourceLimit, "opencode-bridge: request budget exhausted");
    }
    if (body.size() > config_.max_input_bytes) {
        throw Error(ErrorCode::ResourceLimit, "opencode-bridge: input byte limit exceeded");
    }
    HttpRequest request;
    request.method = std::move(method);
    request.url = std::move(url);
    request.body = std::move(body);
    request.timeout_ms = config_.timeout_ms;
    request.max_response_bytes = config_.max_response_bytes;
    request.headers.emplace_back("Content-Type", "application/json");
    request.headers.emplace_back("Accept", "application/json, text/event-stream");
    if (!config_.username.empty() || !config_.password.empty()) {
        request.headers.emplace_back(
            "Authorization", "Basic " + base64_encode(config_.username + ":" + config_.password));
    }
    ++request_count_;
    HttpResponse response;
    try {
        response = network_.send(request);
    } catch (const Error&) {
        throw Error(ErrorCode::NetworkError, "opencode-bridge: host transport failed");
    } catch (...) {
        throw Error(ErrorCode::NetworkError, "opencode-bridge: host transport failed");
    }
    if (!response.redirect_chain.empty() || (response.status >= 300 && response.status < 400)) {
        throw Error(ErrorCode::SecurityViolation, "opencode-bridge: redirects are forbidden");
    }
    if (!response.final_url.empty() && normalize_url(response.final_url) != normalize_url(request.url)) {
        throw Error(ErrorCode::SecurityViolation, "opencode-bridge: redirects are forbidden");
    }
    if (response.body.size() > config_.max_response_bytes) {
        throw Error(ErrorCode::ResourceLimit, "opencode-bridge: response byte limit exceeded");
    }
    if (!response.ok()) {
        throw Error(ErrorCode::NetworkError,
                    "opencode-bridge: server HTTP status " + std::to_string(response.status));
    }
    return response;
}

bool OpenCodeClient::use_v2_api() const noexcept {
    return config_.api_prefix == "/api";
}

std::string OpenCodeClient::route(std::string_view path) const {
    return normalize_base(config_.base_url) + config_.api_prefix + std::string(path);
}

std::string OpenCodeClient::create_session(bool disable_tools) {
    const HttpResponse response = send("POST", route("/session"),
        disable_tools && use_v2_api()
            ? "{\"permissions\":[{\"action\":\"*\",\"resource\":\"*\",\"effect\":\"deny\"}]}"
            : "{}");
    JsonValue document;
    if (json_parse(response.body, document)) {
        std::string id;
        // v2 envelopes nest the id under "data"; legacy servers return it flat.
        if (json_find_string_at_path(document, {"data", "id"}, id) && !id.empty()) return id;
        if (json_find_string_at_path(document, {"id"}, id) && !id.empty()) return id;
    }
    std::string id;
    for (const char* key : {"id", "session_id", "sessionId"}) {
        if (json_extract_string(response.body, key, id) && !id.empty()) return id;
    }
    throw Error(ErrorCode::ParseError, "opencode-bridge: session id missing in response");
}

// Synchronous prompt against a real OpenCode v2 server: POSTs the prompt,
// then polls the session message list until a new completed assistant message
// appears. Bounded by prompt_wait_ms and the request budget; never echoes
// prompt or answer content in errors.
std::string OpenCodeClient::prompt_v2(const std::string& session_id, std::string_view text) {
    const std::string messages_url = route("/session/" + session_id + "/message?order=desc&limit=128");
    std::set<std::string> baseline;
    {
        const HttpResponse before = send("GET", messages_url, {});
        JsonValue document;
        if (!json_parse(before.body, document)) {
            throw Error(ErrorCode::ParseError, "opencode-bridge: message list is not valid JSON");
        }
        for (const auto& message : parse_assistant_messages(document)) {
            if (!message.id.empty()) baseline.insert(message.id);
        }
    }
    send("POST", route("/session/" + session_id + "/prompt"),
         "{\"text\":" + json_escape(text) + "}");
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(config_.prompt_wait_ms);
    for (;;) {
        const HttpResponse list = send("GET", messages_url, {});
        JsonValue document;
        if (!json_parse(list.body, document)) {
            throw Error(ErrorCode::ParseError, "opencode-bridge: message list is not valid JSON");
        }
        for (const auto& message : parse_assistant_messages(document)) {
            if (message.id.empty() || baseline.count(message.id) != 0) continue;
            if (message.failed || (message.idle && !message.succeeded)) {
                throw Error(ErrorCode::PluginError, "opencode-bridge: agent run did not succeed");
            }
            if (!message.idle && message.completed && !message.text.empty()) return message.text;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            throw Error(ErrorCode::Timeout, "opencode-bridge: timed out waiting for agent reply");
        }
        const auto remaining = deadline - std::chrono::steady_clock::now();
        const auto step = std::chrono::milliseconds(1500);
        std::this_thread::sleep_for(remaining < step ? remaining : step);
    }
}

std::string OpenCodeClient::prompt(const std::string& session_id, std::string_view text,
                                   const std::vector<std::string>& tools) {
    check_session_id(session_id);
    if (text.empty() || text.size() > config_.max_input_bytes) invalid("prompt text");
    for (const auto& tool : tools) {
        if (!header_safe(tool) || tool.size() > 256) invalid("tool name");
    }
    if (use_v2_api()) {
        // The v2 prompt route takes text only; silently dropping tool names
        // would mislead the caller into thinking the agent received them.
        if (!tools.empty()) {
            throw Error(ErrorCode::InvalidArgument,
                        "opencode-bridge: tools are not supported by the v2 prompt route");
        }
        return prompt_v2(session_id, text);
    }
    std::string body = "{\"text\":" + json_escape(text);
    if (!tools.empty()) {
        body += ",\"tools\":[";
        for (std::size_t i = 0; i < tools.size(); ++i) {
            if (i) body += ",";
            body += json_escape(tools[i]);
        }
        body += "]";
    }
    body += "}";
    const std::string url = normalize_base(config_.base_url) + "/session/" + session_id + "/message";
    const HttpResponse response = send("POST", url, body);
    std::string answer;
    for (const char* key : {"text", "message", "content", "answer"}) {
        if (json_extract_string(response.body, key, answer) && !answer.empty()) return answer;
    }
    if (!response.body.empty() && response.body.size() <= config_.max_response_bytes) return response.body;
    throw Error(ErrorCode::ParseError, "opencode-bridge: answer missing in response");
}

std::string OpenCodeClient::prompt_message(const std::string& session_id, std::string_view text) {
    check_session_id(session_id);
    if (text.empty() || text.size() > config_.max_input_bytes) invalid("prompt text");
    const auto response = send("POST", route("/session/" + percent_encode(session_id) + "/message"),
        "{\"parts\":[{\"type\":\"text\",\"text\":" + json_escape(text) + "}],"
        "\"tools\":{\"bash\":false,\"edit\":false,\"write\":false,\"read\":false,"
        "\"glob\":false,\"grep\":false,\"webfetch\":false,\"task\":false}}");
    JsonValue root;
    if (!json_parse(response.body, root) || root.type != JsonValue::Type::Object) {
        throw Error(ErrorCode::ParseError, "opencode-bridge: invalid message response");
    }
    const JsonValue* parts = nullptr;
    for (const auto& field : root.fields) {
        if (field.first == "info") {
            for (const auto& item : field.second->fields) {
                if (item.first == "error" && item.second->type != JsonValue::Type::Null) {
                    throw Error(ErrorCode::PluginError, "opencode-bridge: assistant failed");
                }
            }
        }
        if (field.first == "parts") parts = field.second.get();
    }
    std::string answer;
    if (parts && parts->type == JsonValue::Type::Array) {
        for (const auto& part : parts->items) {
            std::string type, content;
            if (json_find_string_at_path(part, {"type"}, type) && type == "text" &&
                json_find_string_at_path(part, {"text"}, content)) answer += content;
        }
    }
    if (answer.empty()) throw Error(ErrorCode::ParseError, "opencode-bridge: assistant text missing");
    return answer;
}

std::string OpenCodeClient::prompt_async(const std::string& session_id, std::string_view text) {
    check_session_id(session_id);
    if (text.empty() || text.size() > config_.max_input_bytes) invalid("prompt text");
    const std::string url = use_v2_api() ? route("/session/" + session_id + "/prompt")
                                            : normalize_base(config_.base_url) + "/session/" + session_id +
                                                  "/prompt_async";
    const HttpResponse response = send("POST", url, "{\"text\":" + json_escape(text) + "}");
    std::string operation;
    for (const char* key : {"operation_id", "operationId", "id"}) {
        if (json_extract_string(response.body, key, operation) && !operation.empty()) return operation;
    }
    if (!response.body.empty()) return response.body;
    throw Error(ErrorCode::ParseError, "opencode-bridge: operation id missing in response");
}

std::vector<std::string> OpenCodeClient::stream_events(const std::string& session_id) {
    check_session_id(session_id);
    const std::string url = use_v2_api() ? route("/event")
                                            : normalize_base(config_.base_url) + "/event?sessionId=" +
                                                  percent_encode_impl(session_id);
    const HttpResponse response = send("GET", url, {});
    return parse_sse_events(response.body);
}

void OpenCodeClient::abort(const std::string& session_id) {
    check_session_id(session_id);
    const std::string url = use_v2_api() ? route("/session/" + session_id + "/interrupt")
                                            : normalize_base(config_.base_url) + "/session/" + session_id +
                                                  "/abort";
    send("POST", url, "{}");
}

std::string OpenCodeClient::scrape_with_prompt(const std::string& session_id,
                                               const PageSnapshot& snapshot,
                                               std::string_view instructions,
                                               std::string_view schema) {
    const std::string full = build_scrape_prompt(snapshot, instructions, schema);
    if (full.size() > config_.max_input_bytes) {
        throw Error(ErrorCode::ResourceLimit, "opencode-bridge: input byte limit exceeded");
    }
    const std::string answer = prompt(session_id, full);
    if (!schema.empty()) {
        // Validate shape only: the answer must be a JSON object. Content stays
        // caller data; no values are logged or echoed in errors.
        std::size_t at = answer.find_first_not_of(" \t\r\n");
        if (at == std::string::npos || answer[at] != '{' ||
            answer.find_last_not_of(" \t\r\n") == std::string::npos ||
            answer[answer.find_last_not_of(" \t\r\n")] != '}') {
            throw Error(ErrorCode::ParseError, "opencode-bridge: answer is not a JSON object");
        }
    }
    return answer;
}

}  // namespace prowsetk::plugins::opencode_bridge
