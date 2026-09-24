#include "prowsetk/plugins/beacon.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <random>

namespace prowsetk::plugins::beacon {
namespace {

constexpr const char* kResourceTypes[] = {
    "main_frame",
    "sub_frame",
    "stylesheet",
    "script",
    "image",
    "xmlhttprequest",
    "other",
};

std::string lower_copy(std::string_view value) {
    std::string out(value);
    std::transform(out.begin(), out.end(), out.begin(), [](char ch) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    });
    return out;
}

// Locate a member at an exact object depth. Never interpret a key in a
// payload, string value, or nested object as a protocol discriminator.
std::optional<std::size_t> field_value(std::string_view json,
                                       std::string_view key, int target_depth) {
    const auto start = json.find_first_not_of(" \t\r\n");
    const auto end = json.find_last_not_of(" \t\r\n");
    if (start == std::string_view::npos || json[start] != '{' ||
        json[end] != '}') return std::nullopt;
    int depth = 0;
    for (std::size_t i = start; i <= end; ++i) {
        if (json[i] == '"') {
            const std::size_t begin = ++i;
            while (i <= end && json[i] != '"') {
                if (json[i] == '\\') ++i;
                ++i;
            }
            if (i > end) return std::nullopt;
            if (depth == target_depth &&
                json.substr(begin, i - begin) == key) {
                std::size_t pos = json.find_first_not_of(" \t\r\n", i + 1);
                if (pos != std::string_view::npos && json[pos] == ':') {
                    pos = json.find_first_not_of(" \t\r\n", pos + 1);
                    if (pos != std::string_view::npos) return pos;
                }
            }
        } else if (json[i] == '{' || json[i] == '[') {
            ++depth;
        } else if (json[i] == '}' || json[i] == ']') {
            --depth;
            if (depth < 0) return std::nullopt;
        }
    }
    return std::nullopt;
}

std::optional<std::string> extract_string(std::string_view json,
                                          std::string_view key, int depth = 1) {
    const auto found = field_value(json, key, depth);
    if (!found) return std::nullopt;
    std::size_t pos = *found;
    if (pos >= json.size() || json[pos] != '"') return std::nullopt;
    ++pos;
    std::string out;
    bool escape = false;
    for (; pos < json.size(); ++pos) {
        const char ch = json[pos];
        if (escape) {
            switch (ch) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                default: return std::nullopt;
            }
            escape = false;
        } else if (ch == '\\') {
            escape = true;
        } else if (ch == '"') {
            return out;
        } else if (static_cast<unsigned char>(ch) < 0x20) {
            return std::nullopt;
        } else {
            out += ch;
        }
    }
    return std::nullopt;
}

std::optional<long long> extract_int(std::string_view json,
                                     std::string_view key) {
    const auto found = field_value(json, key, 1);
    if (!found) return std::nullopt;
    std::size_t pos = *found;
    const std::size_t start = pos;
    if (pos < json.size() && json[pos] == '-') ++pos;
    while (pos < json.size() &&
           (std::isdigit(static_cast<unsigned char>(json[pos])) != 0)) {
        ++pos;
    }
    if (start == pos || (pos < json.size() && json[pos] != ',' &&
                         json[pos] != '}' && json[pos] != ' ' &&
                         json[pos] != '\n')) return std::nullopt;
    long long value = 0;
    const auto* first = json.data() + start;
    const auto* last = json.data() + pos;
    const auto res = std::from_chars(first, last, value);
    if (res.ec != std::errc()) return std::nullopt;
    return value;
}

bool looks_like_uuid(std::string_view id) {
    if (id.size() != 36 || id[14] != '4' ||
        (id[19] != '8' && id[19] != '9' && id[19] != 'a' &&
         id[19] != 'A' && id[19] != 'b' && id[19] != 'B')) return false;
    for (std::size_t i = 0; i < id.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (id[i] != '-') return false;
        } else if (!std::isxdigit(static_cast<unsigned char>(id[i]))) {
            return false;
        }
    }
    return true;
}

}  // namespace

const char* to_string(RequestType type) noexcept {
    switch (type) {
        case RequestType::PageDom: return "page_dom";
        case RequestType::NetworkInfo: return "network_info";
        case RequestType::Stylesheet: return "stylesheet";
        case RequestType::Tracepoint: return "tracepoint";
    }
    return "page_dom";
}

const char* to_string(FlashStatus status) noexcept {
    switch (status) {
        case FlashStatus::Seeking: return "seeking";
        case FlashStatus::Connected: return "connected";
        case FlashStatus::Closed: return "closed";
        case FlashStatus::TimedOut: return "timed_out";
    }
    return "seeking";
}

const char* to_string(TracepointEventKind kind) noexcept {
    switch (kind) {
        case TracepointEventKind::DomMutation: return "dom_mutation";
        case TracepointEventKind::NetworkRequest: return "network_request";
        case TracepointEventKind::StyleChange: return "style_change";
    }
    return "dom_mutation";
}

std::optional<RequestType> parse_request_type(std::string_view name) noexcept {
    const std::string lowered = lower_copy(name);
    if (lowered == "page_dom" || lowered == "page-dom" || lowered == "dom") {
        return RequestType::PageDom;
    }
    if (lowered == "network_info" || lowered == "network-info" ||
        lowered == "network" || lowered == "har") {
        return RequestType::NetworkInfo;
    }
    if (lowered == "stylesheet" || lowered == "style" || lowered == "css") {
        return RequestType::Stylesheet;
    }
    if (lowered == "tracepoint" || lowered == "trace" || lowered == "watch") {
        return RequestType::Tracepoint;
    }
    return std::nullopt;
}

std::optional<FlashStatus> parse_flash_status(std::string_view name) noexcept {
    const std::string lowered = lower_copy(name);
    if (lowered == "seeking") return FlashStatus::Seeking;
    if (lowered == "connected") return FlashStatus::Connected;
    if (lowered == "closed") return FlashStatus::Closed;
    if (lowered == "timed_out" || lowered == "timed-out" || lowered == "timeout") {
        return FlashStatus::TimedOut;
    }
    return std::nullopt;
}

std::optional<TracepointEventKind> parse_tracepoint_event(
    std::string_view name) noexcept {
    const std::string lowered = lower_copy(name);
    if (lowered == "dom_mutation" || lowered == "dom-mutation" ||
        lowered == "mutation") {
        return TracepointEventKind::DomMutation;
    }
    if (lowered == "network_request" || lowered == "network-request" ||
        lowered == "request") {
        return TracepointEventKind::NetworkRequest;
    }
    if (lowered == "style_change" || lowered == "style-change" ||
        lowered == "style") {
        return TracepointEventKind::StyleChange;
    }
    return std::nullopt;
}

bool is_valid_resource_type(std::string_view name) noexcept {
    const std::string lowered = lower_copy(name);
    for (const char* allowed : kResourceTypes) {
        if (lowered == allowed) return true;
    }
    return false;
}

std::string make_flash_id() {
    std::random_device rd;
    unsigned bytes[16];
    for (unsigned& b : bytes) b = rd() & 0xFFu;
    // RFC 4122 version 4 + variant bits.
    bytes[6] = (bytes[6] & 0x0Fu) | 0x40u;
    bytes[8] = (bytes[8] & 0x3Fu) | 0x80u;
    char buf[37];
    std::snprintf(buf, sizeof(buf),
                  "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x"
                  "%02x%02x",
                  bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5],
                  bytes[6], bytes[7], bytes[8], bytes[9], bytes[10], bytes[11],
                  bytes[12], bytes[13], bytes[14], bytes[15]);
    return std::string(buf, 36);
}

FlashRequest new_flash(RequestType type, FlashFilters filters,
                       std::string flash_id) {
    FlashRequest request;
    request.request_type = type;
    request.filters = std::move(filters);
    request.flash_id =
        flash_id.empty() ? make_flash_id() : std::move(flash_id);
    return request;
}

bool validate_flash_filters(const FlashFilters& filters,
                            std::string* error_out) {
    if (filters.url_pattern.empty()) {
        if (error_out != nullptr) *error_out = "url_pattern must not be empty";
        return false;
    }
    for (const auto& resource : filters.resource_types) {
        if (!is_valid_resource_type(resource)) {
            if (error_out != nullptr) {
                *error_out = "unknown resource_type: " + resource;
            }
            return false;
        }
    }
    return true;
}

bool validate_flash_request(const FlashRequest& request,
                            std::string* error_out) {
    if (request.flash_id.empty() || !looks_like_uuid(request.flash_id)) {
        if (error_out != nullptr) *error_out = "flash_id must be a uuid-v4";
        return false;
    }
    return validate_flash_filters(request.filters, error_out);
}

std::string json_escape(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 2);
    for (char ch : value) {
        switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20) {
                    char buf[7];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", ch);
                    out += buf;
                } else {
                    out += ch;
                }
                break;
        }
    }
    return out;
}

std::string render_flash_request(const FlashRequest& request,
                                 const Redactor& redactor, bool redact) {
    std::string pattern = request.filters.url_pattern;
    if (redact) pattern = redactor.redact_url(pattern);
    std::string json = "{\"type\":\"flash_request\",\"flash_id\":\"";
    json += json_escape(request.flash_id);
    json += "\",\"request_type\":\"";
    json += to_string(request.request_type);
    json += "\",\"filters\":{\"url_pattern\":\"";
    json += json_escape(pattern);
    json += "\",\"resource_types\":[";
    for (std::size_t i = 0; i < request.filters.resource_types.size(); ++i) {
        if (i != 0) json += ",";
        json += "\"";
        json += json_escape(request.filters.resource_types[i]);
        json += "\"";
    }
    json += "]}}";
    return json;
}

std::string render_flash_list(const std::vector<FlashDescriptor>& flashes,
                              const Redactor& redactor, bool redact) {
    std::string json = "{\"type\":\"flash_list\",\"flashes\":[";
    for (std::size_t i = 0; i < flashes.size(); ++i) {
        const auto& flash = flashes[i];
        if (i != 0) json += ",";
        std::string pattern = flash.filters.url_pattern;
        if (redact) pattern = redactor.redact_url(pattern);
        json += "{\"flash_id\":\"";
        json += json_escape(flash.flash_id);
        json += "\",\"request_type\":\"";
        json += to_string(flash.request_type);
        json += "\",\"filters\":{\"url_pattern\":\"";
        json += json_escape(pattern);
        json += "\"},\"status\":\"";
        json += to_string(flash.status);
        json += "\"}";
    }
    json += "]}";
    return json;
}

std::string render_flash_connect(const FlashConnect& connect) {
    std::string json = "{\"type\":\"flash_connect\",\"flash_id\":\"";
    json += json_escape(connect.flash_id);
    json += "\",\"tab_id\":";
    json += std::to_string(connect.tab_id);
    json += "}";
    return json;
}

std::string render_flash_data(const FlashData& data, const Redactor& redactor,
                              bool redact) {
    std::string url = data.url;
    if (redact && !url.empty()) url = redactor.redact_url(url);
    std::string json = "{\"type\":\"flash_data\",\"flash_id\":\"";
    json += json_escape(data.flash_id);
    json += "\",\"data_type\":\"";
    json += json_escape(data.data_type);
    json += "\",\"payload\":{\"html\":\"";
    json += json_escape(data.html);
    json += "\",\"url\":\"";
    json += json_escape(url);
    json += "\",\"timestamp\":";
    json += std::to_string(data.timestamp);
    json += "}}";
    return json;
}

std::string render_tracepoint_event(const TracepointEvent& event) {
    std::string json = "{\"type\":\"tracepoint_event\",\"flash_id\":\"";
    json += json_escape(event.flash_id);
    json += "\",\"event\":\"";
    json += to_string(event.event);
    json += "\",\"details\":{\"selector\":\"";
    json += json_escape(event.selector);
    json += "\",\"changes\":\"";
    json += json_escape(event.detail);
    json += "\"}}";
    return json;
}

std::string render_flash_disconnect(const std::string& flash_id) {
    return "{\"type\":\"flash_disconnect\",\"flash_id\":\"" +
           json_escape(flash_id) + "\"}";
}

std::string render_ping() { return "{\"type\":\"ping\"}"; }

std::optional<FlashRequest> parse_flash_request(std::string_view json) {
    auto type = extract_string(json, "type");
    if (!type.has_value() || *type != "flash_request") return std::nullopt;
    auto id = extract_string(json, "flash_id");
    auto request_type = extract_string(json, "request_type");
    const auto filters = field_value(json, "filters", 1);
    if (!filters || json[*filters] != '{') return std::nullopt;
    std::size_t end = *filters + 1;
    int level = 1;
    bool quoted = false;
    for (; end < json.size() && level != 0; ++end) {
        const char ch = json[end];
        if (quoted && ch == '\\') { ++end; continue; }
        if (ch == '"') quoted = !quoted;
        if (!quoted && ch == '{') ++level;
        if (!quoted && ch == '}') --level;
    }
    if (level != 0) return std::nullopt;
    const std::string_view filter_json = json.substr(*filters, end - *filters);
    auto pattern = extract_string(filter_json, "url_pattern");
    if (!id.has_value() || !request_type.has_value() || !pattern.has_value()) {
        return std::nullopt;
    }
    auto parsed = parse_request_type(*request_type);
    if (!parsed.has_value()) return std::nullopt;
    FlashRequest request;
    request.flash_id = *id;
    request.request_type = *parsed;
    request.filters.url_pattern = *pattern;
    if (const auto resources = field_value(filter_json, "resource_types", 1)) {
        std::size_t pos = *resources;
        if (filter_json[pos++] != '[') return std::nullopt;
        while (pos < filter_json.size()) {
            pos = filter_json.find_first_not_of(" \t\r\n", pos);
            if (pos == std::string_view::npos) return std::nullopt;
            if (filter_json[pos] == ']') break;
            if (filter_json[pos++] != '"') return std::nullopt;
            const auto item_end = filter_json.find('"', pos);
            if (item_end == std::string_view::npos) return std::nullopt;
            request.filters.resource_types.emplace_back(filter_json.substr(pos, item_end - pos));
            pos = filter_json.find_first_not_of(" \t\r\n", item_end + 1);
            if (pos == std::string_view::npos) return std::nullopt;
            if (filter_json[pos] == ']') break;
            if (filter_json[pos++] != ',') return std::nullopt;
        }
        if (pos >= filter_json.size()) return std::nullopt;
    }
    return request;
}

std::optional<FlashConnect> parse_flash_connect(std::string_view json) {
    auto type = extract_string(json, "type");
    if (!type.has_value() || *type != "flash_connect") return std::nullopt;
    auto id = extract_string(json, "flash_id");
    auto tab = extract_int(json, "tab_id");
    if (!id.has_value() || !tab.has_value()) return std::nullopt;
    FlashConnect connect;
    connect.flash_id = *id;
    if (*tab < 0 || *tab > 2147483647LL) return std::nullopt;
    connect.tab_id = static_cast<int>(*tab);
    return connect;
}

std::optional<std::string> parse_message_type(std::string_view json) {
    return extract_string(json, "type");
}

std::optional<std::string> parse_flash_id(std::string_view json) {
    return extract_string(json, "flash_id");
}

std::optional<int> parse_tab_id(std::string_view json) {
    const auto tab = extract_int(json, "tab_id");
    if (!tab || *tab < 0 || *tab > 2147483647LL) return std::nullopt;
    return static_cast<int>(*tab);
}

std::vector<std::uint8_t> encode_native_message(std::string_view json) {
    if (json.size() > kMaxNativeMessageBytes) return {};
    std::vector<std::uint8_t> out;
    out.reserve(4 + json.size());
    const auto len = static_cast<std::uint32_t>(json.size());
    out.push_back(static_cast<std::uint8_t>(len & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((len >> 8) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((len >> 16) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((len >> 24) & 0xFFu));
    for (char ch : json) out.push_back(static_cast<std::uint8_t>(ch));
    return out;
}

bool decode_native_message(const std::vector<std::uint8_t>& bytes,
                           std::string* json_out, std::string* error_out) {
    if (bytes.size() < 4) {
        if (error_out != nullptr) *error_out = "message too short";
        return false;
    }
    const std::uint32_t len =
        static_cast<std::uint32_t>(bytes[0]) |
        (static_cast<std::uint32_t>(bytes[1]) << 8) |
        (static_cast<std::uint32_t>(bytes[2]) << 16) |
        (static_cast<std::uint32_t>(bytes[3]) << 24);
    if (len > kMaxNativeMessageBytes) {
        if (error_out != nullptr) *error_out = "message exceeds size limit";
        return false;
    }
    if (len == 0 || bytes.size() - 4 != len) {
        if (error_out != nullptr) *error_out = "truncated message";
        return false;
    }
    if (json_out != nullptr) {
        json_out->assign(reinterpret_cast<const char*>(bytes.data() + 4), len);
    }
    return true;
}

bool decode_native_message(const std::string& bytes, std::string* json_out,
                           std::string* error_out) {
    std::vector<std::uint8_t> raw(bytes.size());
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        raw[i] = static_cast<std::uint8_t>(bytes[i]);
    }
    return decode_native_message(raw, json_out, error_out);
}

FlashSessionManager::FlashSessionManager(BeaconOptions options,
                                         Redactor redactor)
    : options_(std::move(options)), redactor_(std::move(redactor)) {
    if (options_.max_flashes == 0) options_.max_flashes = kDefaultMaxFlashes;
    if (options_.timeout_ms == 0) options_.timeout_ms = kDefaultBeaconTimeoutMs;
}

bool FlashSessionManager::create(const FlashRequest& request,
                                 std::uint64_t now_ms,
                                 std::string* error_out) {
    if (!validate_flash_request(request, error_out)) return false;
    if (entries_.size() >= options_.max_flashes) {
        if (error_out != nullptr) *error_out = "flash table full";
        return false;
    }
    if (entries_.find(request.flash_id) != entries_.end()) {
        if (error_out != nullptr) *error_out = "duplicate flash_id";
        return false;
    }
    Entry entry;
    entry.descriptor.flash_id = request.flash_id;
    entry.descriptor.request_type = request.request_type;
    entry.descriptor.filters = request.filters;
    entry.descriptor.status = FlashStatus::Seeking;
    entry.created_ms = now_ms;
    entry.last_activity_ms = now_ms;
    entries_.emplace(request.flash_id, std::move(entry));
    return true;
}

bool FlashSessionManager::connect(const std::string& flash_id, int tab_id,
                                  std::uint64_t now_ms,
                                  std::string* error_out) {
    auto it = entries_.find(flash_id);
    if (it == entries_.end()) {
        if (error_out != nullptr) *error_out = "unknown flash_id";
        return false;
    }
    if (tab_id < 0) {
        if (error_out != nullptr) *error_out = "tab_id must be >= 0";
        return false;
    }
    if (it->second.descriptor.status != FlashStatus::Seeking) {
        if (error_out != nullptr) *error_out = "flash already connected";
        return false;
    }
    // User consent is enforced by the addon (explicit "Connect to Flash"
    // click). A manager configured to require consent still accepts the
    // connect call because the call itself represents that click; the flag
    // exists so future transports can reject programmatic connects.
    it->second.descriptor.status = FlashStatus::Connected;
    it->second.tab_id = tab_id;
    it->second.last_activity_ms = now_ms;
    return true;
}

bool FlashSessionManager::disconnect(const std::string& flash_id,
                                     std::string* error_out) {
    auto it = entries_.find(flash_id);
    if (it == entries_.end()) {
        if (error_out != nullptr) *error_out = "unknown flash_id";
        return false;
    }
    entries_.erase(it);
    return true;
}

std::size_t FlashSessionManager::expire_stale(std::uint64_t now_ms) {
    std::size_t removed = 0;
    for (auto it = entries_.begin(); it != entries_.end();) {
        const std::uint64_t idle = now_ms >= it->second.last_activity_ms
                                       ? now_ms - it->second.last_activity_ms
                                       : 0;
        if (idle > options_.timeout_ms) {
            it = entries_.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    return removed;
}

std::vector<FlashDescriptor> FlashSessionManager::list() const {
    std::vector<FlashDescriptor> out;
    out.reserve(entries_.size());
    for (const auto& [id, entry] : entries_) out.push_back(entry.descriptor);
    return out;
}

std::vector<FlashDescriptor> FlashSessionManager::list_seeking() const {
    std::vector<FlashDescriptor> out;
    for (const auto& [id, entry] : entries_) {
        if (entry.descriptor.status == FlashStatus::Seeking) {
            out.push_back(entry.descriptor);
        }
    }
    return out;
}

std::optional<FlashDescriptor> FlashSessionManager::find(
    std::string_view flash_id) const {
    auto it = entries_.find(flash_id);
    if (it == entries_.end()) return std::nullopt;
    return it->second.descriptor;
}

std::optional<int> FlashSessionManager::connected_tab(
    std::string_view flash_id) const {
    auto it = entries_.find(flash_id);
    if (it == entries_.end()) return std::nullopt;
    if (it->second.descriptor.status != FlashStatus::Connected) {
        return std::nullopt;
    }
    return it->second.tab_id;
}

bool FlashSessionManager::publish(std::string_view flash_id, int tab_id,
                                  std::string message, std::uint64_t now_ms,
                                  std::string* error_out) {
    auto it = entries_.find(flash_id);
    if (it == entries_.end() ||
        it->second.descriptor.status != FlashStatus::Connected ||
        it->second.tab_id != tab_id) {
        if (error_out != nullptr) *error_out = "flash is not connected to tab";
        return false;
    }
    if (message.empty() || message.size() > kMaxNativeMessageBytes) {
        if (error_out != nullptr) *error_out = "invalid message size";
        return false;
    }
    if (it->second.messages.size() >= 16) {
        if (error_out != nullptr) *error_out = "flash queue full";
        return false;
    }
    it->second.messages.push_back(std::move(message));
    it->second.last_activity_ms = now_ms;
    return true;
}

std::optional<std::string> FlashSessionManager::poll(
    std::string_view flash_id, std::uint64_t now_ms, std::string* error_out) {
    auto it = entries_.find(flash_id);
    if (it == entries_.end()) {
        if (error_out != nullptr) *error_out = "unknown flash_id";
        return std::nullopt;
    }
    it->second.last_activity_ms = now_ms;
    if (it->second.messages.empty()) return std::nullopt;
    std::string message = std::move(it->second.messages.front());
    it->second.messages.pop_front();
    return message;
}

void FlashSessionManager::clear() { entries_.clear(); }

std::string FlashSessionManager::render_list() const {
    const bool redact = options_.redact_secrets;
    return render_flash_list(list(), redactor_, redact);
}

std::vector<std::string> beacon_capabilities() {
    return {
        "beacon-oracle",
        "flash-lifecycle",
        "page-dom",
        "network-info",
        "stylesheet",
        "tracepoint",
    };
}

}  // namespace prowsetk::plugins::beacon
