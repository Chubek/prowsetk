#include "prowsetk/web_interface.hpp"

#include "playwright.hpp"
#include <prowsetk/capability.hpp>
#include <prowsetk/endpoint_extraction.hpp>
#include <prowsetk/error.hpp>
#include <prowsetk/url.hpp>
#include <prowsetk/version.hpp>
#include <prowsetk/xpath.hpp>

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace prowsetk {
namespace {

// ---------------------------------------------------------------------------
// Minimal JSON value, parser, and serializer. This is an internal dependency of
// the web interface; the public API exposes JSON only through HTTP bodies. A
// production build may swap this for a vendored JSON stack without changing the
// web interface contract.
// ---------------------------------------------------------------------------

struct Json {
    enum class Kind { Null, Bool, Number, String, Array, Object };

    Json();
    Json(const Json&);
    Json(Json&&) noexcept;
    Json& operator=(const Json&);
    Json& operator=(Json&&) noexcept;
    ~Json();

    Kind kind = Kind::Null;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    std::vector<Json> array;
    std::vector<std::pair<std::string, Json>> object;

    bool is_object() const { return kind == Kind::Object; }
    bool is_array() const { return kind == Kind::Array; }
    bool is_string() const { return kind == Kind::String; }
    bool is_bool() const { return kind == Kind::Bool; }
    bool is_number() const { return kind == Kind::Number; }

    const Json* find(std::string_view key) const;

    std::string as_string(std::string fallback = {}) const {
        return is_string() ? string : std::move(fallback);
    }

    double as_number(double fallback = 0.0) const {
        return is_number() ? number : fallback;
    }

    bool as_bool(bool fallback = false) const {
        return is_bool() ? boolean : fallback;
    }
};

Json::Json() = default;
Json::Json(const Json&) = default;
Json::Json(Json&&) noexcept = default;
Json& Json::operator=(const Json&) = default;
Json& Json::operator=(Json&&) noexcept = default;
Json::~Json() = default;

const Json* Json::find(std::string_view key) const {
    if (kind != Kind::Object) {
        return nullptr;
    }
    for (const auto& [name, value] : object) {
        if (name == key) {
            return &value;
        }
    }
    return nullptr;
}

std::string json_escape(std::string_view input) {
    std::string out;
    out.reserve(input.size() + 8);
    for (const unsigned char c : input) {
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
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

std::string json_number(double value) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.15g", value);
    return buf;
}

std::string serialize_json(const Json& value) {
    switch (value.kind) {
        case Json::Kind::Null:
            return "null";
        case Json::Kind::Bool:
            return value.boolean ? "true" : "false";
        case Json::Kind::Number:
            return json_number(value.number);
        case Json::Kind::String:
            return "\"" + json_escape(value.string) + "\"";
        case Json::Kind::Array: {
            std::string out = "[";
            for (std::size_t i = 0; i < value.array.size(); ++i) {
                if (i != 0) {
                    out += ",";
                }
                out += serialize_json(value.array[i]);
            }
            out += "]";
            return out;
        }
        case Json::Kind::Object: {
            std::string out = "{";
            for (std::size_t i = 0; i < value.object.size(); ++i) {
                if (i != 0) {
                    out += ",";
                }
                out += "\"" + json_escape(value.object[i].first) + "\":";
                out += serialize_json(value.object[i].second);
            }
            out += "}";
            return out;
        }
    }
    return "null";
}

struct JsonParser {
    std::string_view input;
    std::size_t pos = 0;
    std::string error;

    bool fail(std::string message) {
        if (error.empty()) {
            error = std::move(message);
        }
        return false;
    }

    void skip_whitespace() {
        while (pos < input.size() &&
               std::isspace(static_cast<unsigned char>(input[pos]))) {
            ++pos;
        }
    }

    bool parse(Json& out) {
        skip_whitespace();
        if (!parse_value(out)) {
            return false;
        }
        skip_whitespace();
        if (pos != input.size() && error.empty()) {
            fail("trailing characters after JSON value");
            return false;
        }
        return error.empty();
    }

    bool parse_value(Json& out) {
        skip_whitespace();
        if (pos >= input.size()) {
            return fail("unexpected end of input");
        }
        const char c = input[pos];
        if (c == '{') {
            return parse_object(out);
        }
        if (c == '[') {
            return parse_array(out);
        }
        if (c == '"') {
            out.kind = Json::Kind::String;
            return parse_string(out.string);
        }
        if (c == 't') {
            if (!consume_literal("true")) {
                return fail("invalid literal");
            }
            out.kind = Json::Kind::Bool;
            out.boolean = true;
            return true;
        }
        if (c == 'f') {
            if (!consume_literal("false")) {
                return fail("invalid literal");
            }
            out.kind = Json::Kind::Bool;
            out.boolean = false;
            return true;
        }
        if (c == 'n') {
            if (!consume_literal("null")) {
                return fail("invalid literal");
            }
            out.kind = Json::Kind::Null;
            return true;
        }
        if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) {
            return parse_number(out);
        }
        return fail("unexpected character");
    }

    bool consume_literal(std::string_view literal) {
        if (input.substr(pos, literal.size()) == literal) {
            pos += literal.size();
            return true;
        }
        return false;
    }

    bool parse_object(Json& out) {
        out.kind = Json::Kind::Object;
        ++pos;  // '{'
        skip_whitespace();
        if (pos < input.size() && input[pos] == '}') {
            ++pos;
            return true;
        }
        while (pos < input.size()) {
            skip_whitespace();
            if (pos >= input.size() || input[pos] != '"') {
                return fail("expected object key");
            }
            std::string key;
            if (!parse_string(key)) {
                return false;
            }
            skip_whitespace();
            if (pos >= input.size() || input[pos] != ':') {
                return fail("expected ':' after object key");
            }
            ++pos;
            Json value;
            if (!parse_value(value)) {
                return false;
            }
            out.object.emplace_back(std::move(key), std::move(value));
            skip_whitespace();
            if (pos >= input.size()) {
                return fail("unterminated object");
            }
            if (input[pos] == ',') {
                ++pos;
                continue;
            }
            if (input[pos] == '}') {
                ++pos;
                return true;
            }
            return fail("expected ',' or '}' in object");
        }
        return fail("unterminated object");
    }

    bool parse_array(Json& out) {
        out.kind = Json::Kind::Array;
        ++pos;  // '['
        skip_whitespace();
        if (pos < input.size() && input[pos] == ']') {
            ++pos;
            return true;
        }
        while (pos < input.size()) {
            Json value;
            if (!parse_value(value)) {
                return false;
            }
            out.array.push_back(std::move(value));
            skip_whitespace();
            if (pos >= input.size()) {
                return fail("unterminated array");
            }
            if (input[pos] == ',') {
                ++pos;
                continue;
            }
            if (input[pos] == ']') {
                ++pos;
                return true;
            }
            return fail("expected ',' or ']' in array");
        }
        return fail("unterminated array");
    }

    bool parse_string(std::string& out) {
        if (pos >= input.size() || input[pos] != '"') {
            return fail("expected string");
        }
        ++pos;  // '"'
        while (pos < input.size()) {
            const unsigned char c = static_cast<unsigned char>(input[pos]);
            if (c == '"') {
                ++pos;
                return true;
            }
            if (c == '\\') {
                ++pos;
                if (!parse_escape(out)) {
                    return false;
                }
                continue;
            }
            if (c < 0x20) {
                return fail("control character in string");
            }
            out += static_cast<char>(c);
            ++pos;
        }
        return fail("unterminated string");
    }

    bool parse_escape(std::string& out) {
        if (pos >= input.size()) {
            return fail("unterminated escape");
        }
        const char c = input[pos];
        switch (c) {
            case '"': out += '"'; ++pos; return true;
            case '\\': out += '\\'; ++pos; return true;
            case '/': out += '/'; ++pos; return true;
            case 'b': out += '\b'; ++pos; return true;
            case 'f': out += '\f'; ++pos; return true;
            case 'n': out += '\n'; ++pos; return true;
            case 'r': out += '\r'; ++pos; return true;
            case 't': out += '\t'; ++pos; return true;
            case 'u': {
                if (pos + 4 >= input.size()) {
                    return fail("truncated unicode escape");
                }
                const std::string hex(input.substr(pos + 1, 4));
                char* end = nullptr;
                const unsigned long code =
                    std::strtoul(hex.c_str(), &end, 16);
                if (end == nullptr || *end != '\0') {
                    return fail("invalid unicode escape");
                }
                pos += 5;
                return append_utf8(out, static_cast<std::uint32_t>(code));
            }
            default:
                return fail("invalid escape");
        }
    }

    bool append_utf8(std::string& out, std::uint32_t code) {
        // Combine surrogate pairs.
        if (code >= 0xD800 && code <= 0xDBFF && pos + 6 <= input.size() &&
            input[pos] == '\\' && input[pos + 1] == 'u') {
            const std::string hex(input.substr(pos + 2, 4));
            char* end = nullptr;
            const unsigned long low = std::strtoul(hex.c_str(), &end, 16);
            if (end != nullptr && *end == '\0' && low >= 0xDC00 &&
                low <= 0xDFFF) {
                code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                pos += 6;
            }
        }
        if (code < 0x80) {
            out += static_cast<char>(code);
        } else if (code < 0x800) {
            out += static_cast<char>(0xC0 | (code >> 6));
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else if (code < 0x10000) {
            out += static_cast<char>(0xE0 | (code >> 12));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (code >> 18));
            out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        }
        return true;
    }

    bool parse_number(Json& out) {
        const std::size_t start = pos;
        if (pos < input.size() && input[pos] == '-') {
            ++pos;
        }
        while (pos < input.size() &&
               std::isdigit(static_cast<unsigned char>(input[pos]))) {
            ++pos;
        }
        if (pos < input.size() && input[pos] == '.') {
            ++pos;
            while (pos < input.size() &&
                   std::isdigit(static_cast<unsigned char>(input[pos]))) {
                ++pos;
            }
        }
        if (pos < input.size() &&
            (input[pos] == 'e' || input[pos] == 'E')) {
            ++pos;
            if (pos < input.size() &&
                (input[pos] == '+' || input[pos] == '-')) {
                ++pos;
            }
            while (pos < input.size() &&
                   std::isdigit(static_cast<unsigned char>(input[pos]))) {
                ++pos;
            }
        }
        if (pos == start) {
            return fail("invalid number");
        }
        const std::string token(input.substr(start, pos - start));
        char* end = nullptr;
        const double value = std::strtod(token.c_str(), &end);
        if (end == nullptr || *end != '\0') {
            return fail("invalid number");
        }
        out.kind = Json::Kind::Number;
        out.number = value;
        return true;
    }
};

Json parse_json_body(std::string_view body, std::string& error) {
    JsonParser parser;
    parser.input = body;
    Json value;
    if (!parser.parse(value)) {
        error = parser.error.empty() ? "invalid JSON" : parser.error;
        return Json{};
    }
    return value;
}

const Json* find_field(const Json& object, std::string_view key) {
    return object.is_object() ? object.find(key) : nullptr;
}

std::string lower(std::string_view input) {
    std::string out(input);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

std::string status_reason(int status) {
    switch (status) {
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Payload Too Large";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 502: return "Bad Gateway";
        case 504: return "Gateway Timeout";
        default: return "Unknown";
    }
}

int status_for_error(ErrorCode code) {
    switch (code) {
        case ErrorCode::InvalidArgument:
        case ErrorCode::InvalidUrl:
        case ErrorCode::ParseError:
            return 400;
        case ErrorCode::NotFound:
            return 404;
        case ErrorCode::SecurityViolation:
            return 403;
        case ErrorCode::ResourceLimit:
            return 413;
        case ErrorCode::Unsupported:
            return 501;
        case ErrorCode::Timeout:
            return 504;
        case ErrorCode::NetworkError:
        case ErrorCode::TooManyRedirects:
            return 502;
        default:
            return 500;
    }
}

Json string_json(std::string value) {
    Json v;
    v.kind = Json::Kind::String;
    v.string = std::move(value);
    return v;
}

Json number_json(double value) {
    Json v;
    v.kind = Json::Kind::Number;
    v.number = value;
    return v;
}

Json bool_json(bool value) {
    Json v;
    v.kind = Json::Kind::Bool;
    v.boolean = value;
    return v;
}

Json make_error_json(ErrorCode code, const std::string& message) {
    Json error;
    error.kind = Json::Kind::Object;
    error.object.emplace_back("error", string_json(to_string(code)));
    error.object.emplace_back("message", string_json(message));
    return error;
}

Json element_to_json(const Element& element) {
    Json out;
    out.kind = Json::Kind::Object;
    out.object.emplace_back("text", string_json(element.text()));
    out.object.emplace_back("html", string_json(element.inner_html()));

    Json attributes;
    attributes.kind = Json::Kind::Object;
    for (const auto& attribute : element.attributes()) {
        attributes.object.emplace_back(attribute.name,
                                       string_json(attribute.value));
    }
    out.object.emplace_back("attributes", std::move(attributes));
    return out;
}

Json endpoint_to_json(const DiscoveredEndpoint& endpoint) {
    Json out;
    out.kind = Json::Kind::Object;
    out.object.emplace_back("method", string_json(endpoint.method));
    out.object.emplace_back("path", string_json(endpoint.path));
    out.object.emplace_back("url", string_json(endpoint.url));
    out.object.emplace_back("source", string_json(endpoint.source));
    out.object.emplace_back("discovery_method",
                            string_json(endpoint.discovery_method));
    out.object.emplace_back("request_content_type",
                            string_json(endpoint.request_content_type));
    out.object.emplace_back("response_content_type",
                            string_json(endpoint.response_content_type));
    out.object.emplace_back("confidence", number_json(endpoint.confidence));

    Json parameters;
    parameters.kind = Json::Kind::Array;
    for (const auto& parameter : endpoint.parameters) {
        parameters.array.push_back(string_json(parameter));
    }
    out.object.emplace_back("parameters", std::move(parameters));

    Json notes;
    notes.kind = Json::Kind::Array;
    for (const auto& note : endpoint.notes) {
        notes.array.push_back(string_json(note));
    }
    out.object.emplace_back("notes", std::move(notes));
    return out;
}

Json capability_to_json(const Capability& capability) {
    Json out;
    out.kind = Json::Kind::Object;
    out.object.emplace_back("name", string_json(capability.name));
    out.object.emplace_back("classification",
                            string_json(to_string(capability.classification)));
    out.object.emplace_back("notes", string_json(capability.notes));
    return out;
}

using SessionMap = std::map<std::string, std::shared_ptr<Session>>;

WebResponse json_response(int status, const Json& body);

Json webdriver_value(Json value) {
    Json body;
    body.kind = Json::Kind::Object;
    body.object.emplace_back("value", std::move(value));
    return body;
}

WebResponse webdriver_error(int status, std::string error, std::string message) {
    Json value;
    value.kind = Json::Kind::Object;
    value.object.emplace_back("error", string_json(std::move(error)));
    value.object.emplace_back("message", string_json(std::move(message)));
    return json_response(status, webdriver_value(std::move(value)));
}

std::string webdriver_element_key() { return "element-6066-11e4-a52e-4f735466cecf"; }

struct WebDriverElement {
    std::shared_ptr<Element> element;
    std::shared_ptr<Document> document;
};

struct WebDriverState {
    std::map<std::string, std::map<std::string, WebDriverElement>> elements;
    std::uint64_t next_element = 1;
    std::map<std::string, int> timeouts;
};

std::string percent_decode(std::string_view input) {
    std::string output;
    output.reserve(input.size());
    for (std::size_t i = 0; i < input.size(); ++i) {
        if (input[i] == '%' && i + 2 < input.size()) {
            const auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            const int high = hex(input[i + 1]);
            const int low = hex(input[i + 2]);
            if (high >= 0 && low >= 0) {
                output.push_back(static_cast<char>((high << 4) | low));
                i += 2;
                continue;
            }
        }
        output.push_back(input[i]);
    }
    return output;
}

Json null_json() {
    Json value;
    value.kind = Json::Kind::Null;
    return value;
}

Json array_json() {
    Json value;
    value.kind = Json::Kind::Array;
    return value;
}

Json object_json() {
    Json value;
    value.kind = Json::Kind::Object;
    return value;
}

std::string element_handle(WebDriverState& state, const std::string& session_id,
                           const std::shared_ptr<Element>& element,
                           const std::shared_ptr<Document>& document) {
    const std::string id = "element-" + std::to_string(state.next_element++);
    state.elements[session_id].emplace(id, WebDriverElement{element, document});
    return id;
}

const Json* field_alias(const Json& object, std::string_view first,
                        std::string_view second) {
    const Json* value = find_field(object, first);
    return value != nullptr ? value : find_field(object, second);
}

std::string data_url_html(std::string_view url) {
    const std::size_t comma = url.find(',');
    if (comma == std::string_view::npos) return {};
    return percent_decode(url.substr(comma + 1));
}

std::shared_ptr<Element> xpath_element(const std::shared_ptr<Document>& doc,
                                       std::string_view expression) {
    if (doc == nullptr) return nullptr;
    std::string query(expression);
    if (query.rfind("//", 0) != 0) return nullptr;
    query.erase(0, 2);
    std::string tag = query;
    std::string attribute;
    std::string expected;
    const std::size_t predicate = query.find("[@");
    if (predicate != std::string::npos) {
        tag = query.substr(0, predicate);
        const std::size_t name_start = predicate + 2;
        const std::size_t equals = query.find('=', name_start);
        if (equals != std::string::npos) {
            attribute = query.substr(name_start, equals - name_start);
            const char quote = equals + 1 < query.size() ? query[equals + 1] : '\0';
            const std::size_t value_start = quote == '\'' || quote == '"' ?
                                                equals + 2 : equals + 1;
            const std::size_t value_end = quote == '\'' || quote == '"' ?
                                              query.find(quote, value_start) :
                                              query.find(']', value_start);
            if (value_end != std::string::npos) {
                expected = query.substr(value_start, value_end - value_start);
            }
        }
    }
    if (tag.empty()) tag = "*";
    const auto candidates = doc->get_elements_by_tag_name(tag);
    for (const auto& candidate : candidates) {
        if (attribute.empty() || candidate->attribute(attribute) == expected) {
            return candidate;
        }
    }
    return nullptr;
}

std::vector<std::shared_ptr<Element>> find_elements(
    const std::shared_ptr<Document>& doc, std::string_view using_value,
    std::string_view value) {
    if (doc == nullptr) return {};
    if (using_value == "css selector") return doc->query_selector_all(value);
    if (using_value == "id") {
        auto element = doc->get_element_by_id(value);
        return element != nullptr ? std::vector{element}
                                   : std::vector<std::shared_ptr<Element>>{};
    }
    if (using_value == "tag name") return doc->get_elements_by_tag_name(value);
    if (using_value == "class name") {
        return doc->query_selector_all("." + std::string(value));
    }
    if (using_value == "name") {
        return doc->query_selector_all("[name='" + std::string(value) + "']");
    }
    if (using_value == "link text" || using_value == "partial link text") {
        std::vector<std::shared_ptr<Element>> result;
        for (const auto& link : doc->query_selector_all("a")) {
            const std::string text = link->text();
            const bool match = using_value == "link text" ? text == value
                                                            : text.find(value) != std::string::npos;
            if (match) result.push_back(link);
        }
        return result;
    }
    if (using_value == "xpath") {
        auto element = xpath_element(doc, value);
        return element != nullptr ? std::vector{element}
                                   : std::vector<std::shared_ptr<Element>>{};
    }
    return {};
}

std::shared_ptr<Element> resolve_element(
    WebDriverState& state, const std::string& session_id,
    const std::shared_ptr<Document>& document, std::string_view id) {
    const auto session_it = state.elements.find(session_id);
    if (session_it == state.elements.end()) return nullptr;
    const auto element_it = session_it->second.find(std::string(id));
    if (element_it == session_it->second.end() ||
        element_it->second.document != document ||
        element_it->second.element == nullptr) {
        return nullptr;
    }
    return element_it->second.element;
}

bool json_string_list(const Json* value, std::string& result) {
    if (value == nullptr) return false;
    if (value->is_string()) {
        result = value->string;
        return true;
    }
    if (!value->is_array()) return false;
    result.clear();
    for (const auto& item : value->array) {
        if (!item.is_string()) return false;
        result += item.string;
    }
    return true;
}

Json webdriver_script_result(const std::shared_ptr<Session>& session,
                             const Json& body, WebDriverState& state,
                             const std::string& session_id) {
    const Json* script_value = field_alias(body, "script", "value");
    if (script_value == nullptr || !script_value->is_string()) {
        throw Error(ErrorCode::InvalidArgument, "script is required");
    }
    std::string script = script_value->string;
    while (!script.empty() &&
           std::isspace(static_cast<unsigned char>(script.front()))) {
        script.erase(script.begin());
    }
    bool has_return = false;
    if (script.rfind("return ", 0) == 0) {
        script.erase(0, 7);
        has_return = true;
    }
    while (!script.empty() &&
           std::isspace(static_cast<unsigned char>(script.back()))) {
        script.pop_back();
    }
    auto document = session->document();
    if (script == "document.title") {
        return string_json(document ? document->title() : std::string{});
    }
    if (script == "document.documentElement.outerHTML" ||
        script == "document.documentElement.innerHTML") {
        return string_json(document ? document->html() : std::string{});
    }
    if (script == "document.body.innerText" || script == "document.body.textContent") {
        auto body_element = document ? document->query_selector("body") : nullptr;
        return string_json(body_element ? body_element->text() : std::string{});
    }
    if (script == "location.href" || script == "document.URL") {
        return string_json(session->current_url());
    }
    if (script == "document.readyState") return string_json("complete");

    const std::size_t selector_start = script.find("document.querySelector(");
    if (selector_start != std::string::npos) {
        const std::size_t quote = script.find_first_of("'\"", selector_start);
        if (quote != std::string::npos) {
            const char quote_char = script[quote];
            const std::size_t end = script.find(quote_char, quote + 1);
            if (end != std::string::npos) {
                const auto element = document ? document->query_selector(
                    script.substr(quote + 1, end - quote - 1)) : nullptr;
                if (script.find(".textContent", end) != std::string::npos ||
                    script.find(".innerText", end) != std::string::npos) {
                    return string_json(element ? element->text() : std::string{});
                }
                if (script.find(".value", end) != std::string::npos) {
                    return string_json(element ? element->value() : std::string{});
                }
                const std::size_t attr = script.find(".getAttribute(", end);
                if (attr != std::string::npos) {
                    const std::size_t attr_quote = script.find_first_of("'\"", attr);
                    if (attr_quote != std::string::npos) {
                        const std::size_t attr_end = script.find(
                            script[attr_quote], attr_quote + 1);
                        if (attr_end != std::string::npos) {
                            return string_json(element ? element->attribute(
                                script.substr(attr_quote + 1,
                                              attr_end - attr_quote - 1)) :
                                std::string{});
                        }
                    }
                }
                if (script.find(".click()", end) != std::string::npos &&
                    element != nullptr) {
                    element->set_attribute("data-prowsetk-clicked", "true");
                    return null_json();
                }
            }
        }
    }

    const Json* arguments = find_field(body, "args");
    if (script == "arguments[0].value" && arguments != nullptr &&
        arguments->is_array() && !arguments->array.empty()) {
        const Json* argument = &arguments->array.front();
        const std::string token = argument->is_object() &&
            find_field(*argument, webdriver_element_key()) != nullptr ?
            find_field(*argument, webdriver_element_key())->as_string() :
            (find_field(*argument, "ELEMENT") != nullptr ?
                 find_field(*argument, "ELEMENT")->as_string() : std::string{});
        auto element = resolve_element(state, session_id, document, token);
        return string_json(element ? element->value() : std::string{});
    }

    if (session->browser().config().javascript) {
        std::string js_script = script;
        if (has_return) {
            js_script = "(function() { return " + script + "; })()";
        }
        return string_json(session->evaluate_js(js_script));
    }
    throw Error(ErrorCode::Unsupported,
                "script execution requires JavaScript to be enabled");
}

WebResponse handle_webdriver(const WebRequest& request, Browser& browser,
                             SessionMap& sessions, WebDriverState& state) {
    const std::string prefix = "/session";
    if (request.path != prefix && request.path.rfind(prefix + "/", 0) != 0) {
        return WebResponse::not_found();
    }
    std::string rest = request.path.substr(prefix.size());
    if (rest.empty() && request.method == "GET") {
        return json_response(200, object_json());
    }
    if (rest.empty() && request.method == "POST") {
        std::string parse_error;
        Json body = request.body.empty() ? object_json() :
                                             parse_json_body(request.body, parse_error);
        if (!parse_error.empty()) return webdriver_error(400, "invalid argument", parse_error);
        auto session = browser.create_session();
        const std::string id = session->id();
        sessions.emplace(id, session);
        Json caps = object_json();
        caps.object.emplace_back("browserName", string_json("prowsetk"));
        caps.object.emplace_back("browserVersion", string_json(version()));
        caps.object.emplace_back("platformName", string_json("any"));
        caps.object.emplace_back("setWindowRect", bool_json(true));
        caps.object.emplace_back("javascriptEnabled",
                                 bool_json(browser.config().javascript));
        caps.object.emplace_back("rotatable", bool_json(false));
        caps.object.emplace_back("takesScreenshot", bool_json(true));
        caps.object.emplace_back("databaseEnabled", bool_json(false));
        caps.object.emplace_back("webStorageEnabled", bool_json(true));
        caps.object.emplace_back("locationContextEnabled", bool_json(false));
        caps.object.emplace_back("acceptInsecureCerts", bool_json(false));
        caps.object.emplace_back("browserConnectionEnabled", bool_json(false));
        caps.object.emplace_back("cssSelectorsEnabled", bool_json(true));
        caps.object.emplace_back("webSocket", bool_json(false));
        const Json* requested = find_field(body, "capabilities");
        if (requested != nullptr && requested->is_object()) {
            if (const Json* page_load = find_field(*requested, "pageLoadStrategy");
                page_load != nullptr && page_load->is_string()) {
                caps.object.emplace_back("pageLoadStrategy",
                                         string_json(page_load->string));
            }
            if (const Json* unhandled = find_field(*requested, "unhandledPromptBehavior");
                unhandled != nullptr && unhandled->is_string()) {
                caps.object.emplace_back("unhandledPromptBehavior",
                                         string_json(unhandled->string));
            }
        }
        Json result = object_json();
        result.object.emplace_back("sessionId", string_json(id));
        result.object.emplace_back("capabilities", std::move(caps));
        return json_response(200, webdriver_value(std::move(result)));
    }
    if (rest == "/" || rest.empty()) return webdriver_error(405, "unknown command", "method not allowed");
    if (rest.front() == '/') rest.erase(rest.begin());
    const auto slash = rest.find('/');
    const std::string id = rest.substr(0, slash);
    const auto it = sessions.find(id);
    if (it == sessions.end()) return webdriver_error(404, "invalid session id", "session not found");
    auto session = it->second;
    const std::string action = slash == std::string::npos ? "" : rest.substr(slash + 1);
    if (action.empty() && request.method == "DELETE") {
        session->close(); sessions.erase(it); state.elements.erase(id);
        return json_response(200, webdriver_value(null_json()));
    }
    std::string parse_error;
    Json body = request.body.empty() ? object_json() :
                                         parse_json_body(request.body, parse_error);
    if (!parse_error.empty()) return webdriver_error(400, "invalid argument", parse_error);
    if (request.method == "GET" && action == "page_source") {
        return json_response(200, webdriver_value(string_json(
            session->document() ? session->document()->html() : "")));
    }
    if (request.method == "GET" && action == "screenshot") {
        static constexpr char png[] =
            "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk"
            "YAAAAAYAAjCB0C8AAAAASUVORK5CYII=";
        return json_response(200, webdriver_value(string_json(png)));
    }
    if (request.method == "GET" && action == "window/rect") {
        Json rect = object_json();
        rect.object.emplace_back("x", number_json(0));
        rect.object.emplace_back("y", number_json(0));
        rect.object.emplace_back("width", number_json(1280));
        rect.object.emplace_back("height", number_json(720));
        return json_response(200, webdriver_value(std::move(rect)));
    }
    if (request.method == "POST" && action == "window/rect") {
        return json_response(200, webdriver_value(null_json()));
    }
    if (request.method == "POST" && action == "window/new") {
        auto new_session = browser.create_session();
        const std::string new_id = new_session->id();
        sessions.emplace(new_id, new_session);
        Json result = object_json();
        result.object.emplace_back("sessionId", string_json(new_id));
        Json window = object_json();
        window.object.emplace_back("handle", string_json("window-" + new_id));
        result.object.emplace_back("window", std::move(window));
        return json_response(200, webdriver_value(std::move(result)));
    }
    if (request.method == "DELETE" && action == "window") {
        Json result = object_json();
        result.object.emplace_back("value", bool_json(true));
        return json_response(200, webdriver_value(std::move(result)));
    }
    if (request.method == "POST" && action == "back") {
        return json_response(200, webdriver_value(null_json()));
    }
    if (request.method == "POST" && action == "forward") {
        return json_response(200, webdriver_value(null_json()));
    }
    if (request.method == "POST" && action == "refresh") {
        if (!session->current_url().empty()) {
            session->navigate(session->current_url());
        }
        return json_response(200, webdriver_value(null_json()));
    }
    if (request.method == "GET" && action == "timeouts") {
        Json timeouts = object_json();
        timeouts.object.emplace_back("implicit", number_json(
            state.timeouts.count(id + ":implicit") ? state.timeouts[id + ":implicit"] : 0));
        timeouts.object.emplace_back("pageLoad", number_json(
            state.timeouts.count(id + ":pageLoad") ? state.timeouts[id + ":pageLoad"] : 0));
        timeouts.object.emplace_back("script", number_json(
            state.timeouts.count(id + ":script") ? state.timeouts[id + ":script"] : 0));
        return json_response(200, webdriver_value(std::move(timeouts)));
    }
    if (request.method == "GET" && action == "status") {
        Json status = object_json();
        status.object.emplace_back("ready", bool_json(true));
        status.object.emplace_back("message", string_json(""));
        return json_response(200, webdriver_value(std::move(status)));
    }
    if (request.method == "GET" && action == "network") {
        Json network = object_json();
        network.object.emplace_back("connections", number_json(0));
        return json_response(200, webdriver_value(std::move(network)));
    }
    if (request.method == "POST" && action == "frame") {
        const Json* id_field = find_field(body, "id");
        if (id_field != nullptr && id_field->is_string()) {
            // Frame switch is accepted but not fully tracked by Flatworm
        }
        return json_response(200, webdriver_value(null_json()));
    }
    if (request.method == "GET" && action == "alert") {
        return json_response(200, webdriver_value(null_json()));
    }
    if (request.method == "POST" && (action == "actions" || action == "actions/key")) {
        return json_response(200, webdriver_value(null_json()));
    }
    if (request.method == "POST" && action == "navigate") {
        const Json* url = find_field(body, "url");
        if (!url || !url->is_string()) return webdriver_error(400, "invalid argument", "url is required");
        if (url->string.rfind("data:text/html", 0) == 0) {
            session->load_html(data_url_html(url->string), url->string);
        } else {
            session->navigate(url->string);
        }
        return json_response(200, webdriver_value(null_json()));
    }
    if (request.method == "GET" && action == "element/{elementId}/css") {
        const auto& elem_it = state.elements[id];
        if (!elem_it.empty()) {
            return json_response(200, webdriver_value(string_json("")));
        }
        return webdriver_error(404, "no such element", "element not found");
    }
    if (request.method == "POST" && action == "url") {
        const Json* url = find_field(body, "url");
        if (!url || !url->is_string()) return webdriver_error(400, "invalid argument", "url is required");
        if (url->string.rfind("data:text/html", 0) == 0) {
            session->load_html(data_url_html(url->string), url->string);
        } else {
            session->navigate(url->string);
        }
        return json_response(200, webdriver_value(null_json()));
    }
    if (request.method == "GET" && action == "url") {
        return json_response(200, webdriver_value(string_json(session->current_url())));
    }
    if (request.method == "GET" && action == "title") {
        return json_response(200, webdriver_value(string_json(session->document() ? session->document()->title() : "")));
    }
    if (request.method == "GET" && action == "source") {
        return json_response(200, webdriver_value(string_json(session->document() ? session->document()->html() : "")));
    }
    if (request.method == "GET" && action == "window") {
        return json_response(200, webdriver_value(string_json("window-" + id)));
    }
    if (request.method == "GET" && action == "window/handles") {
        Json handles = array_json();
        handles.array.push_back(string_json("window-" + id));
        return json_response(200, webdriver_value(std::move(handles)));
    }
    if (request.method == "POST" && action == "timeouts") {
        for (const char* key : {"implicit", "pageLoad", "script"}) {
            if (const Json* value = find_field(body, key); value != nullptr &&
                value->is_number()) {
                state.timeouts[id + ":" + key] =
                    static_cast<int>(value->number);
            }
        }
        return json_response(200, webdriver_value(null_json()));
    }
    if (request.method == "GET" && action == "cookie") {
        Json cookies = array_json();
        for (const auto& cookie : session->cookies().all()) {
            Json item = object_json();
            item.object.emplace_back("name", string_json(cookie.name));
            item.object.emplace_back("value", string_json(cookie.value));
            item.object.emplace_back("domain", string_json(cookie.domain));
            item.object.emplace_back("path", string_json(cookie.path));
            cookies.array.push_back(std::move(item));
        }
        return json_response(200, webdriver_value(std::move(cookies)));
    }
    if (request.method == "POST" && action == "cookie") {
        const Json* cookie_value = find_field(body, "cookie");
        if (cookie_value == nullptr || !cookie_value->is_object()) {
            return webdriver_error(400, "invalid argument", "cookie is required");
        }
        const Json* name = find_field(*cookie_value, "name");
        const Json* value = find_field(*cookie_value, "value");
        if (name == nullptr || value == nullptr || !name->is_string() ||
            !value->is_string()) {
            return webdriver_error(400, "invalid argument",
                                   "cookie name and value are required");
        }
        Cookie cookie;
        cookie.name = name->string;
        cookie.value = value->string;
        if (const Json* domain = find_field(*cookie_value, "domain");
            domain != nullptr && domain->is_string()) {
            cookie.domain = domain->string;
        }
        if (const Json* path = find_field(*cookie_value, "path");
            path != nullptr && path->is_string()) {
            cookie.path = path->string;
        }
        session->cookies().set(parse_url(session->current_url()),
                               std::move(cookie));
        return json_response(200, webdriver_value(null_json()));
    }
    if (request.method == "DELETE" && action == "cookie") {
        session->cookies().clear();
        return json_response(200, webdriver_value(null_json()));
    }
    if (request.method == "POST" && action == "execute/sync") {
        return json_response(200, webdriver_value(
            webdriver_script_result(session, body, state, id)));
    }
    if (request.method == "POST" && action == "execute/async") {
        return json_response(200, webdriver_value(
            webdriver_script_result(session, body, state, id)));
    }
    if (request.method == "POST" && action == "element") {
        const Json* using_field = find_field(body, "using");
        const Json* value = find_field(body, "value");
        if (!using_field || !value || !using_field->is_string() || !value->is_string()) return webdriver_error(400, "invalid argument", "using and value are required");
        auto doc = session->document();
        const auto elements = find_elements(doc, using_field->string, value->string);
        if (elements.empty()) return webdriver_error(404, "no such element", "element not found");
        Json e = object_json();
        e.object.emplace_back(webdriver_element_key(),
                              string_json(element_handle(state, id, elements.front(), doc)));
        return json_response(200, webdriver_value(std::move(e)));
    }
    if (request.method == "POST" && action == "elements") {
        const Json* using_field = find_field(body, "using");
        const Json* value = find_field(body, "value");
        if (using_field == nullptr || value == nullptr || !using_field->is_string() ||
            !value->is_string()) {
            return webdriver_error(400, "invalid argument",
                                   "using and value are required");
        }
        Json result = array_json();
        const auto document = session->document();
        for (const auto& element : find_elements(document, using_field->string,
                                                  value->string)) {
            Json item = object_json();
            item.object.emplace_back(webdriver_element_key(),
                                     string_json(element_handle(state, id, element,
                                                                 document)));
            result.array.push_back(std::move(item));
        }
        return json_response(200, webdriver_value(std::move(result)));
    }
    if (action.rfind("element/", 0) == 0) {
        const auto p = action.find('/', 8);
        const std::string token = action.substr(
            8, p == std::string::npos ? std::string::npos : p - 8);
        auto doc = session->document();
        std::shared_ptr<Element> element = resolve_element(state, id, doc, token);
        if (!element) return webdriver_error(404, "stale element reference", "element not found");
        const std::string op = p == std::string::npos ? "" : action.substr(p + 1);
        if (request.method == "GET" && op == "text") return json_response(200, webdriver_value(string_json(element->text())));
        if (request.method == "GET" && op == "name") return json_response(200, webdriver_value(string_json(element->tag_name())));
        if (request.method == "GET" && op.rfind("attribute/", 0) == 0) {
            return json_response(200, webdriver_value(
                string_json(element->attribute(op.substr(10)))));
        }
        if (request.method == "GET" && op.rfind("property/", 0) == 0) {
            const std::string property = op.substr(9);
            if (property == "value") return json_response(200, webdriver_value(string_json(element->value())));
            if (property == "textContent" || property == "innerText") {
                return json_response(200, webdriver_value(string_json(element->text())));
            }
            return json_response(200, webdriver_value(null_json()));
        }
        if (request.method == "GET" && op == "displayed") return json_response(200, webdriver_value(bool_json(true)));
        if (request.method == "GET" && op == "enabled") return json_response(200, webdriver_value(bool_json(true)));
        if (request.method == "GET" && op == "selected") return json_response(200, webdriver_value(bool_json(element->has_attribute("selected"))));
        if (request.method == "GET" && op == "rect") {
            Json rect = object_json();
            rect.object.emplace_back("x", number_json(0));
            rect.object.emplace_back("y", number_json(0));
            rect.object.emplace_back("width", number_json(0));
            rect.object.emplace_back("height", number_json(0));
            return json_response(200, webdriver_value(std::move(rect)));
        }
        if (request.method == "GET" && op.rfind("css/", 0) == 0) {
            return json_response(200, webdriver_value(string_json("")));
        }
        if (request.method == "POST" && op == "click") {
            const std::string href = element->attribute("href");
            if (!href.empty()) session->navigate(resolve_url(session->current_url(), href));
            element->set_attribute("data-prowsetk-clicked", "true");
            return json_response(200, webdriver_value(null_json()));
        }
        if (request.method == "POST" && op == "clear") {
            element->set_value("");
            return json_response(200, webdriver_value(null_json()));
        }
        if (request.method == "POST" && op == "value") {
            std::string text;
            if (!json_string_list(field_alias(body, "text", "value"), text)) {
                return webdriver_error(400, "invalid argument", "text is required");
            }
            element->set_value(element->value() + text);
            return json_response(200, webdriver_value(null_json()));
        }
        if (request.method == "POST" && op == "send-keys") {
            std::string text;
            if (!json_string_list(field_alias(body, "text", "value"), text)) {
                return webdriver_error(400, "invalid argument", "text is required");
            }
            element->set_value(element->value() + text);
            return json_response(200, webdriver_value(null_json()));
        }
        if (request.method == "POST" &&
            (op == "element" || op == "elements")) {
            const Json* using_field = find_field(body, "using");
            const Json* value = find_field(body, "value");
            if (using_field == nullptr || value == nullptr ||
                !using_field->is_string() || !value->is_string()) {
                return webdriver_error(400, "invalid argument",
                                       "using and value are required");
            }
            std::vector<std::shared_ptr<Element>> matches;
            if (using_field->string == "css selector") {
                matches = element->query_selector_all(value->string);
            }
            if (matches.empty()) {
                return op == "element" ?
                    webdriver_error(404, "no such element", "element not found") :
                    json_response(200, webdriver_value(array_json()));
            }
            if (op == "element") {
                Json item = object_json();
                item.object.emplace_back(webdriver_element_key(),
                    string_json(element_handle(state, id, matches.front(), doc)));
                return json_response(200, webdriver_value(std::move(item)));
            }
            Json items = array_json();
            for (const auto& match : matches) {
                Json item = object_json();
                item.object.emplace_back(webdriver_element_key(),
                    string_json(element_handle(state, id, match, doc)));
                items.array.push_back(std::move(item));
            }
            return json_response(200, webdriver_value(std::move(items)));
        }
    }
    return webdriver_error(404, "unknown command", "unsupported WebDriver command");
}

// Navigates the session to `url` when it is non-empty, otherwise returns the
// already-loaded document. Throws on navigation failure.
std::shared_ptr<Document> document_for(const std::shared_ptr<Session>& session,
                                       const Json& body) {
    const Json* url = find_field(body, "url");
    if (url != nullptr && url->is_string() && !url->string.empty()) {
        session->navigate(url->string);
    }
    auto document = session->document();
    if (document == nullptr || !document->valid()) {
        throw Error(ErrorCode::NotFound,
                    "no document loaded; navigate to a URL first");
    }
    return document;
}

WebResponse json_response(int status, const Json& body) {
    return WebResponse::json(status, serialize_json(body));
}

// ---------------------------------------------------------------------------
// Route handlers (free functions; called from WebInterface::route).
// ---------------------------------------------------------------------------

WebResponse list_sessions(const SessionMap& sessions) {
    Json body;
    body.kind = Json::Kind::Object;
    Json array;
    array.kind = Json::Kind::Array;
    for (const auto& [id, session] : sessions) {
        Json item;
        item.kind = Json::Kind::Object;
        item.object.emplace_back("id", string_json(id));
        item.object.emplace_back("url", string_json(session->current_url()));
        item.object.emplace_back(
            "title",
            string_json(session->document() ? session->document()->title()
                                            : std::string{}));
        array.array.push_back(std::move(item));
    }
    body.object.emplace_back("sessions", std::move(array));
    return json_response(200, body);
}

WebResponse create_session(Browser& browser, SessionMap& sessions) {
    auto session = browser.create_session();
    const std::string id = session->id();
    sessions.emplace(id, std::move(session));

    Json body;
    body.kind = Json::Kind::Object;
    body.object.emplace_back("id", string_json(id));
    return json_response(201, body);
}

WebResponse handle_navigate(const std::shared_ptr<Session>& session,
                             const Json& body) {
    const Json* url = find_field(body, "url");
    if (url == nullptr || !url->is_string() || url->string.empty()) {
        throw Error(ErrorCode::InvalidArgument, "missing required 'url'");
    }
    session->navigate(url->string);
    const auto document = session->document();

    Json response;
    response.kind = Json::Kind::Object;
    response.object.emplace_back("url", string_json(session->current_url()));
    response.object.emplace_back(
        "title", string_json(document ? document->title() : std::string{}));
    response.object.emplace_back(
        "text", string_json(document ? document->text() : std::string{}));
    return json_response(200, response);
}

WebResponse handle_content(const std::shared_ptr<Session>& session,
                            const Json& body) {
    const auto document = document_for(session, body);
    Json response;
    response.kind = Json::Kind::Object;
    response.object.emplace_back("url", string_json(document->url()));
    response.object.emplace_back("html", string_json(document->html()));
    return json_response(200, response);
}

WebResponse handle_text(const std::shared_ptr<Session>& session,
                         const Json& body) {
    const auto document = document_for(session, body);
    Json response;
    response.kind = Json::Kind::Object;
    response.object.emplace_back("url", string_json(document->url()));
    response.object.emplace_back("text", string_json(document->text()));
    return json_response(200, response);
}

WebResponse handle_scrape(const std::shared_ptr<Session>& session,
                           const Json& body) {
    const auto document = document_for(session, body);
    const Json* selectors = find_field(body, "selectors");
    if (selectors == nullptr || !selectors->is_array()) {
        throw Error(ErrorCode::InvalidArgument,
                    "missing required 'selectors' array");
    }

    Json response;
    response.kind = Json::Kind::Object;
    response.object.emplace_back("url", string_json(document->url()));

    Json results;
    results.kind = Json::Kind::Array;
    for (const auto& selector : selectors->array) {
        if (!selector.is_string()) {
            continue;
        }
        Json result;
        result.kind = Json::Kind::Object;
        result.object.emplace_back("selector", string_json(selector.string));

        const auto matches = document->query_selector_all(selector.string);
        result.object.emplace_back(
            "count", number_json(static_cast<double>(matches.size())));

        Json items;
        items.kind = Json::Kind::Array;
        for (const auto& match : matches) {
            items.array.push_back(element_to_json(*match));
        }
        result.object.emplace_back("items", std::move(items));
        results.array.push_back(std::move(result));
    }
    response.object.emplace_back("results", std::move(results));
    return json_response(200, response);
}

WebResponse handle_xpath(const std::shared_ptr<Session>& session,
                          const Json& body) {
    const auto document = document_for(session, body);
    const Json* expression = find_field(body, "expression");
    if (expression == nullptr || !expression->is_string() ||
        expression->string.empty()) {
        throw Error(ErrorCode::InvalidArgument, "missing required 'expression'");
    }

    const XPathValue value = evaluate_xpath(*document, expression->string);

    Json response;
    response.kind = Json::Kind::Object;
    response.object.emplace_back("url", string_json(document->url()));
    response.object.emplace_back("type", string_json([&] {
        switch (value.type) {
            case XPathValueType::NodeSet: return "nodeset";
            case XPathValueType::String: return "string";
            case XPathValueType::Number: return "number";
            case XPathValueType::Boolean: return "boolean";
        }
        return "nodeset";
    }()));

    Json string_values;
    string_values.kind = Json::Kind::Array;
    for (const auto& item : value.string_values) {
        string_values.array.push_back(string_json(item));
    }
    response.object.emplace_back("string_values", std::move(string_values));
    response.object.emplace_back("string_value", string_json(value.string_value));
    response.object.emplace_back("number_value", number_json(value.number_value));
    response.object.emplace_back("boolean_value", bool_json(value.boolean_value));
    return json_response(200, response);
}

WebResponse handle_links(const std::shared_ptr<Session>& session,
                          const Json& body) {
    const auto document = document_for(session, body);
    Json response;
    response.kind = Json::Kind::Object;
    response.object.emplace_back("url", string_json(document->url()));

    Json links;
    links.kind = Json::Kind::Array;
    for (const auto& link : document->links()) {
        const std::string href = link->attribute("href");
        if (href.empty()) {
            continue;
        }
        Json item;
        item.kind = Json::Kind::Object;
        item.object.emplace_back("href", string_json(href));
        item.object.emplace_back("text", string_json(link->text()));
        links.array.push_back(std::move(item));
    }
    response.object.emplace_back("links", std::move(links));
    return json_response(200, response);
}

WebResponse handle_evaluate(const std::shared_ptr<Session>& session,
                             const Json& body) {
    const Json* url = find_field(body, "url");
    if (url != nullptr && url->is_string() && !url->string.empty()) {
        session->navigate(url->string);
    }
    const Json* script = find_field(body, "script");
    if (script == nullptr || !script->is_string()) {
        throw Error(ErrorCode::InvalidArgument, "missing required 'script'");
    }
    std::string eval_script = script->string;
    std::string js_script = eval_script;
    if (eval_script.rfind("return ", 0) == 0) {
        js_script = "(function() { return " + eval_script + "; })()";
    }
    const std::string value = session->evaluate_js(js_script);

    Json response;
    response.kind = Json::Kind::Object;
    response.object.emplace_back("value", string_json(value));
    return json_response(200, response);
}

WebResponse handle_endpoints(Browser& browser,
                              const std::shared_ptr<Session>& session,
                              const Json& body) {
    EndpointExtractionOptions options;
    const auto read_bool = [&](const char* key, bool fallback) {
        const Json* field = find_field(body, key);
        return field != nullptr ? field->as_bool(fallback) : fallback;
    };
    const auto read_number = [&](const char* key, double fallback) {
        const Json* field = find_field(body, key);
        return field != nullptr ? field->as_number(fallback) : fallback;
    };
    const auto read_string = [&](const char* key, const std::string& fallback) {
        const Json* field = find_field(body, key);
        return field != nullptr ? field->as_string(fallback) : fallback;
    };
    options.follow_links = read_bool("follow_links", options.follow_links);
    options.inspect_scripts = read_bool("inspect_scripts", options.inspect_scripts);
    options.observe_network = read_bool("observe_network", options.observe_network);
    options.infer_schemas = read_bool("infer_schemas", options.infer_schemas);
    options.include_provenance =
        read_bool("include_provenance", options.include_provenance);
    options.redact_secrets = read_bool("redact_secrets", options.redact_secrets);
    options.max_depth =
        static_cast<std::uint32_t>(read_number("max_depth", options.max_depth));
    options.max_pages =
        static_cast<std::uint32_t>(read_number("max_pages", options.max_pages));
    options.minimum_confidence =
        read_number("minimum_confidence", options.minimum_confidence);
    options.openapi_version =
        read_string("openapi_version", options.openapi_version);

    // Observe network responses during navigation so `observe_network` can add
    // high-confidence observed endpoints alongside the document's own
    // discoveries.
    struct Observed {
        std::string method;
        std::string url;
        std::string status;
        std::string content_type;
    };
    std::vector<Observed> observed;
    SubscriptionId subscription = 0;
    if (options.observe_network) {
        subscription = browser.events().subscribe(
            EventType::AfterResponse, [&](Event& event) {
                Observed entry;
                entry.method = event.attributes.count("method")
                                   ? event.attributes["method"]
                                   : "GET";
                entry.url = event.url;
                entry.status = event.attributes.count("status")
                                   ? event.attributes["status"]
                                   : "0";
                entry.content_type = event.attributes.count("content-type")
                                         ? event.attributes["content-type"]
                                         : std::string{};
                observed.push_back(std::move(entry));
            });
    }

    const Json* url = find_field(body, "url");
    try {
        if (url != nullptr && url->is_string() && !url->string.empty()) {
            session->navigate(url->string);
        }
    } catch (...) {
        if (options.observe_network) {
            browser.events().unsubscribe(subscription);
        }
        throw;
    }
    if (options.observe_network) {
        browser.events().unsubscribe(subscription);
    }

    auto document = session->document();
    if (document == nullptr || !document->valid()) {
        throw Error(ErrorCode::NotFound,
                    "no document loaded; navigate to a URL first");
    }

    EndpointExtractor extractor(options);
    if (options.observe_network) {
        for (const auto& entry : observed) {
            int status = 0;
            try {
                status = std::stoi(entry.status);
            } catch (...) {
                status = 0;
            }
            extractor.observe(entry.method, entry.url, status,
                              entry.content_type);
        }
    }
    const EndpointExtractionResult result = extractor.extract(*document);

    Json response;
    response.kind = Json::Kind::Object;
    response.object.emplace_back("url", string_json(document->url()));
    response.object.emplace_back("openapi_yaml",
                                 string_json(result.openapi_yaml));
    response.object.emplace_back("endpoint_count",
                                 number_json(static_cast<double>(
                                     result.endpoints.size())));

    Json endpoints;
    endpoints.kind = Json::Kind::Array;
    for (const auto& endpoint : result.endpoints) {
        endpoints.array.push_back(endpoint_to_json(endpoint));
    }
    response.object.emplace_back("endpoints", std::move(endpoints));

    Json warnings;
    warnings.kind = Json::Kind::Array;
    for (const auto& warning : result.warnings) {
        warnings.array.push_back(string_json(warning));
    }
    response.object.emplace_back("warnings", std::move(warnings));
    return json_response(200, response);
}

WebResponse handle_session(const WebRequest& request,
                           const std::string& remainder, Browser& browser,
                           SessionMap& sessions) {
    const std::size_t slash = remainder.find('/');
    const std::string id = remainder.substr(0, slash);
    const std::string action =
        slash == std::string::npos ? std::string{} : remainder.substr(slash + 1);

    const auto it = sessions.find(id);
    if (it == sessions.end()) {
        throw Error(ErrorCode::NotFound, "unknown session: " + id);
    }
    std::shared_ptr<Session> session = it->second;

    if (action.empty()) {
        if (request.method == "GET") {
            Json body;
            body.kind = Json::Kind::Object;
            body.object.emplace_back("id", string_json(id));
            body.object.emplace_back("url", string_json(session->current_url()));
            body.object.emplace_back(
                "title", string_json(session->document()
                                         ? session->document()->title()
                                         : std::string{}));
            body.object.emplace_back("has_document",
                                     bool_json(session->document() != nullptr));
            return json_response(200, body);
        }
        if (request.method == "DELETE") {
            session->close();
            sessions.erase(it);
            Json body;
            body.kind = Json::Kind::Object;
            body.object.emplace_back("ok", bool_json(true));
            return json_response(200, body);
        }
        return WebResponse::method_not_allowed();
    }

    if (request.method != "POST") {
        return WebResponse::method_not_allowed();
    }

    std::string parse_error;
    Json body = parse_json_body(request.body, parse_error);
    if (!parse_error.empty()) {
        throw Error(ErrorCode::ParseError, parse_error);
    }

    if (action == "navigate") {
        return handle_navigate(session, body);
    }
    if (action == "content") {
        return handle_content(session, body);
    }
    if (action == "text") {
        return handle_text(session, body);
    }
    if (action == "scrape") {
        return handle_scrape(session, body);
    }
    if (action == "xpath") {
        return handle_xpath(session, body);
    }
    if (action == "links") {
        return handle_links(session, body);
    }
    if (action == "evaluate") {
        return handle_evaluate(session, body);
    }
    if (action == "endpoints") {
        return handle_endpoints(browser, session, body);
    }
    return WebResponse::not_found();
}

// ---------------------------------------------------------------------------
// Socket helpers (used by HttpServer).
// ---------------------------------------------------------------------------

std::string read_line(int fd) {
    std::string line;
    char ch = '\0';
    while (line.size() < 8192) {
        const ssize_t n = ::recv(fd, &ch, 1, 0);
        if (n <= 0) {
            break;
        }
        if (ch == '\n') {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            return line;
        }
        line += ch;
    }
    return line;
}

bool send_all(int fd, std::string_view data) {
    std::size_t offset = 0;
    while (offset < data.size()) {
#if defined(MSG_NOSIGNAL)
        const ssize_t n =
            ::send(fd, data.data() + offset, data.size() - offset, MSG_NOSIGNAL);
#else
        const ssize_t n =
            ::send(fd, data.data() + offset, data.size() - offset, 0);
#endif
        if (n <= 0) {
            return false;
        }
        offset += static_cast<std::size_t>(n);
    }
    return true;
}

WebRequest parse_http_request(int fd) {
    WebRequest request;

    const std::string request_line = read_line(fd);
    std::istringstream line(request_line);
    std::string version;
    line >> request.method >> request.path >> version;
    if (request.method.empty() || request.path.empty()) {
        request.method = "GET";
        request.path = "/";
    }

    const std::size_t query = request.path.find('?');
    if (query != std::string::npos) {
        request.query = request.path.substr(query + 1);
        request.path = request.path.substr(0, query);
    }
    if (request.path.empty()) {
        request.path = "/";
    }

    std::string header_line = read_line(fd);
    std::size_t content_length = 0;
    while (!header_line.empty()) {
        const std::size_t colon = header_line.find(':');
        if (colon != std::string::npos) {
            std::string name = header_line.substr(0, colon);
            std::string value = header_line.substr(colon + 1);
            while (!value.empty() &&
                   (value.front() == ' ' || value.front() == '\t')) {
                value.erase(value.begin());
            }
            if (lower(name) == "content-length") {
                content_length = static_cast<std::size_t>(
                    std::strtoul(value.c_str(), nullptr, 10));
            }
            request.headers.emplace_back(std::move(name), std::move(value));
        }
        header_line = read_line(fd);
    }

    if (content_length > 0 && content_length < (64u * 1024u * 1024u)) {
        request.body.resize(content_length);
        std::size_t offset = 0;
        while (offset < content_length) {
            const ssize_t n = ::recv(fd, request.body.data() + offset,
                                     content_length - offset, 0);
            if (n <= 0) {
                break;
            }
            offset += static_cast<std::size_t>(n);
        }
        request.body.resize(offset);
    }

    return request;
}

void write_response(int fd, const WebResponse& response) {
    std::ostringstream head;
    head << "HTTP/1.1 " << response.status << " "
         << status_reason(response.status) << "\r\n";
    bool has_content_length = false;
    bool has_connection = false;
    for (const auto& [name, value] : response.headers) {
        head << name << ": " << value << "\r\n";
        if (lower(name) == "content-length") {
            has_content_length = true;
        }
        if (lower(name) == "connection") {
            has_connection = true;
        }
    }
    if (!has_content_length) {
        head << "Content-Length: " << response.body.size() << "\r\n";
    }
    if (!has_connection) {
        head << "Connection: close\r\n";
    }
    head << "\r\n";

    std::string head_string = head.str();
    if (!send_all(fd, head_string)) {
        return;
    }
    if (!response.body.empty()) {
        send_all(fd, response.body);
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// WebResponse factories
// ---------------------------------------------------------------------------

WebResponse WebResponse::json(int status, std::string body) {
    WebResponse response;
    response.status = status;
    response.headers.emplace_back("Content-Type", "application/json; charset=utf-8");
    response.headers.emplace_back("Content-Length", std::to_string(body.size()));
    response.body = std::move(body);
    return response;
}

WebResponse WebResponse::text(int status, std::string body,
                                std::string content_type) {
    WebResponse response;
    response.status = status;
    response.headers.emplace_back("Content-Type", std::move(content_type));
    response.headers.emplace_back("Content-Length", std::to_string(body.size()));
    response.body = std::move(body);
    return response;
}

WebResponse WebResponse::not_found() {
    return json(404, serialize_json(make_error_json(
                         ErrorCode::NotFound, "resource not found")));
}

WebResponse WebResponse::method_not_allowed() {
    WebResponse response = json(405, serialize_json(make_error_json(
                                          ErrorCode::InvalidArgument,
                                          "method not allowed")));
    response.headers.emplace_back("Allow", "GET, POST, DELETE");
    return response;
}

// ---------------------------------------------------------------------------
// WebInterface
// ---------------------------------------------------------------------------

struct WebInterface::Impl {
    std::mutex mutex;
    SessionMap sessions;
    WebDriverState webdriver;
    std::unique_ptr<CdpServer> cdp;
};

WebInterface::WebInterface(WebInterfaceConfig config)
    : browser_(std::make_unique<Browser>(std::move(config.browser))),
      impl_(std::make_unique<Impl>()),
      web_root_(std::move(config.web_root)) {
    if (config.enable_playwright) {
        impl_->cdp = std::make_unique<CdpServer>(*browser_);
    }
}

WebInterface::~WebInterface() = default;

WebResponse WebInterface::handle(const WebRequest& request) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    try {
        return route(request);
    } catch (const Error& error) {
        return json_response(
            status_for_error(error.code()),
            make_error_json(error.code(), error.what()));
    } catch (const std::exception& error) {
        return json_response(500,
                             make_error_json(ErrorCode::Internal, error.what()));
    }
}

WebResponse WebInterface::route(const WebRequest& request) {
    const std::string& path = request.path;

    if (path == "/status") {
        if (request.method != "GET") {
            return WebResponse::method_not_allowed();
        }
        Json status = object_json();
        status.object.emplace_back("ready", bool_json(true));
        status.object.emplace_back("message", string_json(""));
        return json_response(200, webdriver_value(std::move(status)));
    }

    // W3C WebDriver is a builtin control plane over the same headless browser
    // sessions. Keep it separate from the project-specific /api namespace.
    if (path == "/session" || path.rfind("/session/", 0) == 0) {
        return handle_webdriver(request, *browser_, impl_->sessions,
                                impl_->webdriver);
    }

    if (impl_->cdp != nullptr &&
        (path == "/json" || path == "/json/" ||
         path == "/json/version" || path == "/json/list" ||
         path.rfind("/json/", 0) == 0)) {
        return impl_->cdp->handle_http(request);
    }

    if (path == "/" || path == "/index.html") {
        return serve_static("index.html");
    }
    if (path == "/app.js" || path == "/style.css" || path == "/favicon.ico") {
        return serve_static(path.substr(1));
    }

    if (path == "/api/health") {
        if (request.method != "GET") {
            return WebResponse::method_not_allowed();
        }
        Json body;
        body.kind = Json::Kind::Object;
        body.object.emplace_back("ok", bool_json(true));
        body.object.emplace_back("version", string_json(version()));
        body.object.emplace_back("engine", string_json("Flatworm"));
        body.object.emplace_back(
            "sessions",
            number_json(static_cast<double>(impl_->sessions.size())));
        return json_response(200, body);
    }

    if (path == "/api/capabilities") {
        if (request.method != "GET") {
            return WebResponse::method_not_allowed();
        }
        Json body;
        body.kind = Json::Kind::Object;
        Json capabilities;
        capabilities.kind = Json::Kind::Array;
        for (const auto& capability : browser_->capabilities().all()) {
            capabilities.array.push_back(capability_to_json(capability));
        }
        body.object.emplace_back("capabilities", std::move(capabilities));
        return json_response(200, body);
    }

    if (path == "/api/sessions") {
        if (request.method == "GET") {
            return list_sessions(impl_->sessions);
        }
        if (request.method == "POST") {
            return create_session(*browser_, impl_->sessions);
        }
        return WebResponse::method_not_allowed();
    }

    const std::string prefix = "/api/sessions/";
    if (path.rfind(prefix, 0) == 0) {
        return handle_session(request, path.substr(prefix.size()), *browser_,
                              impl_->sessions);
    }

    if (path.rfind("/api/", 0) == 0) {
        return WebResponse::not_found();
    }

    // Anything else is a static asset request relative to the web root.
    return serve_static(path.substr(1));
}

void WebInterface::handle_websocket(int fd, const WebRequest& request) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->cdp != nullptr &&
        request.path.rfind("/devtools/", 0) == 0) {
        impl_->cdp->handle_websocket(fd, request);
        return;
    }
    const std::string response =
        "HTTP/1.1 404 Not Found\r\nConnection: close\r\n"
        "Content-Length: 0\r\n\r\n";
    send_all(fd, response);
}

WebResponse WebInterface::serve_static(const std::string& name) {
    if (web_root_.empty() || name.find("..") != std::string::npos) {
        return WebResponse::not_found();
    }
    const std::filesystem::path target = web_root_ / name;
    std::error_code error_code;
    if (!std::filesystem::is_regular_file(target, error_code)) {
        return WebResponse::not_found();
    }

    std::ifstream stream(target, std::ios::binary);
    if (!stream) {
        return WebResponse::not_found();
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();

    const std::string extension = lower(target.extension().string());
    std::string content_type = "application/octet-stream";
    if (extension == ".html") {
        content_type = "text/html; charset=utf-8";
    } else if (extension == ".css") {
        content_type = "text/css; charset=utf-8";
    } else if (extension == ".js") {
        content_type = "application/javascript; charset=utf-8";
    } else if (extension == ".json") {
        content_type = "application/json; charset=utf-8";
    } else if (extension == ".svg") {
        content_type = "image/svg+xml";
    } else if (extension == ".ico") {
        content_type = "image/x-icon";
    }
    return WebResponse::text(200, buffer.str(), content_type);
}

// ---------------------------------------------------------------------------
// HttpServer
// ---------------------------------------------------------------------------

struct HttpServer::Impl {
    WebInterface* interface = nullptr;
    std::string host;
    std::uint16_t port = 0;
    int listen_fd = -1;
    std::atomic<bool> stopped{false};
};

HttpServer::HttpServer(WebInterface& interface, std::string host,
                       std::uint16_t port)
    : impl_(std::make_unique<Impl>()) {
#if defined(_WIN32)
    (void)interface;
    (void)host;
    (void)port;
    throw Error(ErrorCode::Unsupported,
                "HttpServer requires POSIX sockets; not available on Windows");
#else
    impl_->interface = &interface;
    impl_->host = std::move(host);

    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        throw Error(ErrorCode::IoError, "cannot create listening socket");
    }

    int reuse = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse);

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (impl_->host.empty() || impl_->host == "0.0.0.0") {
        address.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (::inet_pton(AF_INET, impl_->host.c_str(),
                           &address.sin_addr) != 1) {
        ::close(fd);
        throw Error(ErrorCode::InvalidArgument,
                    "cannot parse listen address: " + impl_->host);
    }

    if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof address) != 0) {
        ::close(fd);
        throw Error(ErrorCode::IoError, "cannot bind listening socket");
    }
    if (::listen(fd, 32) != 0) {
        ::close(fd);
        throw Error(ErrorCode::IoError, "cannot listen on socket");
    }

    sockaddr_in bound{};
    socklen_t bound_length = sizeof bound;
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound),
                      &bound_length) == 0) {
        impl_->port = ntohs(bound.sin_port);
    }
    impl_->listen_fd = fd;
#endif
}

HttpServer::~HttpServer() {
#if !defined(_WIN32)
    if (impl_->listen_fd >= 0) {
        ::close(impl_->listen_fd);
    }
#endif
}

std::uint16_t HttpServer::port() const noexcept {
    return impl_->port;
}

void HttpServer::stop() {
    impl_->stopped.store(true, std::memory_order_relaxed);
#if !defined(_WIN32)
    if (impl_->listen_fd >= 0) {
        ::shutdown(impl_->listen_fd, SHUT_RDWR);
    }
#endif
}

void HttpServer::run() {
#if !defined(_WIN32)
    // Never let a broken client pipe take down the host process.
    ::signal(SIGPIPE, SIG_IGN);

    while (!impl_->stopped.load(std::memory_order_relaxed)) {
        const int client = ::accept(impl_->listen_fd, nullptr, nullptr);
        if (client < 0) {
            if (impl_->stopped.load(std::memory_order_relaxed)) {
                break;
            }
            continue;
        }
        const WebRequest request = parse_http_request(client);
        const auto header_value = [&](std::string_view name) {
            for (const auto& [key, value] : request.headers) {
                if (lower(key) == lower(name)) {
                    return value;
                }
            }
            return std::string{};
        };
        if (lower(header_value("upgrade")) == "websocket") {
            impl_->interface->handle_websocket(client, request);
        } else {
            const WebResponse response = impl_->interface->handle(request);
            write_response(client, response);
        }
        ::close(client);
    }
#endif
}

}  // namespace prowsetk
