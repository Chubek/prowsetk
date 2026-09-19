#include "playwright.hpp"

#include <prowsetk/document.hpp>
#include <prowsetk/error.hpp>
#include <prowsetk/url.hpp>
#include <prowsetk/version.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace prowsetk {
namespace {

// CDP needs a small JSON envelope, but does not need to expose the web
// interface's parser. Keeping this representation local prevents protocol
// details from entering the public API.
struct Value {
    enum class Kind { Null, Bool, Number, String, Array, Object };
    Kind kind = Kind::Null;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    std::vector<Value> array;
    std::vector<std::pair<std::string, Value>> object;

    const Value* find(std::string_view key) const {
        if (kind != Kind::Object) return nullptr;
        for (const auto& [name, value] : object) {
            if (name == key) return &value;
        }
        return nullptr;
    }
    bool is_string() const { return kind == Kind::String; }
    bool is_number() const { return kind == Kind::Number; }
    bool is_object() const { return kind == Kind::Object; }
};

Value object_value() {
    Value value;
    value.kind = Value::Kind::Object;
    return value;
}

Value array_value() {
    Value value;
    value.kind = Value::Kind::Array;
    return value;
}

Value string_value(std::string value) {
    Value result;
    result.kind = Value::Kind::String;
    result.string = std::move(value);
    return result;
}

Value number_value(double value) {
    Value result;
    result.kind = Value::Kind::Number;
    result.number = value;
    return result;
}

Value bool_value(bool value) {
    Value result;
    result.kind = Value::Kind::Bool;
    result.boolean = value;
    return result;
}

std::string escape_json(std::string_view input) {
    std::string result;
    result.reserve(input.size() + 8);
    for (const unsigned char c : input) {
        switch (c) {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\b': result += "\\b"; break;
            case '\f': result += "\\f"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof buffer, "\\u%04x", c);
                    result += buffer;
                } else {
                    result.push_back(static_cast<char>(c));
                }
        }
    }
    return result;
}

std::string serialize_value(const Value& value) {
    switch (value.kind) {
        case Value::Kind::Null: return "null";
        case Value::Kind::Bool: return value.boolean ? "true" : "false";
        case Value::Kind::Number: {
            char buffer[64];
            std::snprintf(buffer, sizeof buffer, "%.15g", value.number);
            return buffer;
        }
        case Value::Kind::String:
            return "\"" + escape_json(value.string) + "\"";
        case Value::Kind::Array: {
            std::string result = "[";
            for (std::size_t i = 0; i < value.array.size(); ++i) {
                if (i != 0) result += ",";
                result += serialize_value(value.array[i]);
            }
            return result + "]";
        }
        case Value::Kind::Object: {
            std::string result = "{";
            for (std::size_t i = 0; i < value.object.size(); ++i) {
                if (i != 0) result += ",";
                result += "\"" + escape_json(value.object[i].first) + "\":";
                result += serialize_value(value.object[i].second);
            }
            return result + "}";
        }
    }
    return "null";
}

class Parser {
public:
    explicit Parser(std::string_view input) : input_(input) {}

    bool parse(Value& result) {
        skip();
        if (!value(result)) return false;
        skip();
        return position_ == input_.size();
    }

private:
    void skip() {
        while (position_ < input_.size() &&
               std::isspace(static_cast<unsigned char>(input_[position_]))) {
            ++position_;
        }
    }

    bool value(Value& result) {
        skip();
        if (position_ >= input_.size()) return false;
        switch (input_[position_]) {
            case '{': return object(result);
            case '[': return array(result);
            case '"':
                result.kind = Value::Kind::String;
                return string(result.string);
            case 't':
                if (input_.substr(position_, 4) != "true") return false;
                position_ += 4;
                result = bool_value(true);
                return true;
            case 'f':
                if (input_.substr(position_, 5) != "false") return false;
                position_ += 5;
                result = bool_value(false);
                return true;
            case 'n':
                if (input_.substr(position_, 4) != "null") return false;
                position_ += 4;
                result = Value{};
                return true;
            default:
                return number(result);
        }
    }

    bool string(std::string& result) {
        if (position_ >= input_.size() || input_[position_] != '"') return false;
        ++position_;
        while (position_ < input_.size()) {
            const char c = input_[position_++];
            if (c == '"') return true;
            if (c != '\\') {
                result.push_back(c);
                continue;
            }
            if (position_ >= input_.size()) return false;
            const char escaped = input_[position_++];
            switch (escaped) {
                case '"': result.push_back('"'); break;
                case '\\': result.push_back('\\'); break;
                case '/': result.push_back('/'); break;
                case 'b': result.push_back('\b'); break;
                case 'f': result.push_back('\f'); break;
                case 'n': result.push_back('\n'); break;
                case 'r': result.push_back('\r'); break;
                case 't': result.push_back('\t'); break;
                default: return false;
            }
        }
        return false;
    }

    bool number(Value& result) {
        const std::size_t start = position_;
        while (position_ < input_.size() &&
               (std::isdigit(static_cast<unsigned char>(input_[position_])) ||
                input_[position_] == '-' || input_[position_] == '+' ||
                input_[position_] == '.' || input_[position_] == 'e' ||
                input_[position_] == 'E')) {
            ++position_;
        }
        if (start == position_) return false;
        char* end = nullptr;
        const std::string token(input_.substr(start, position_ - start));
        const double parsed = std::strtod(token.c_str(), &end);
        if (end == nullptr || *end != '\0') return false;
        result = number_value(parsed);
        return true;
    }

    bool object(Value& result) {
        result = object_value();
        ++position_;
        skip();
        if (position_ < input_.size() && input_[position_] == '}') {
            ++position_;
            return true;
        }
        while (position_ < input_.size()) {
            std::string key;
            if (!string(key)) return false;
            skip();
            if (position_ >= input_.size() || input_[position_++] != ':') return false;
            Value item;
            if (!value(item)) return false;
            result.object.emplace_back(std::move(key), std::move(item));
            skip();
            if (position_ < input_.size() && input_[position_] == '}') {
                ++position_;
                return true;
            }
            if (position_ >= input_.size() || input_[position_++] != ',') return false;
            skip();
        }
        return false;
    }

    bool array(Value& result) {
        result = array_value();
        ++position_;
        skip();
        if (position_ < input_.size() && input_[position_] == ']') {
            ++position_;
            return true;
        }
        while (position_ < input_.size()) {
            Value item;
            if (!value(item)) return false;
            result.array.push_back(std::move(item));
            skip();
            if (position_ < input_.size() && input_[position_] == ']') {
                ++position_;
                return true;
            }
            if (position_ >= input_.size() || input_[position_++] != ',') return false;
            skip();
        }
        return false;
    }

    std::string_view input_;
    std::size_t position_ = 0;
};

std::string percent_decode(std::string_view input) {
    std::string result;
    result.reserve(input.size());
    for (std::size_t i = 0; i < input.size(); ++i) {
        if (input[i] == '%' && i + 2 < input.size()) {
            const auto hex = [](char c) {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            const int high = hex(input[i + 1]);
            const int low = hex(input[i + 2]);
            if (high >= 0 && low >= 0) {
                result.push_back(static_cast<char>((high << 4) | low));
                i += 2;
                continue;
            }
        }
        result.push_back(input[i]);
    }
    return result;
}

std::string data_html(std::string_view url) {
    const std::size_t comma = url.find(',');
    return comma == std::string_view::npos ? std::string{} :
                                             percent_decode(url.substr(comma + 1));
}

std::string request_header(const WebRequest& request, std::string_view name) {
    for (const auto& [key, value] : request.headers) {
        if (key.size() == name.size()) {
            bool same = true;
            for (std::size_t i = 0; i < key.size(); ++i) {
                if (std::tolower(static_cast<unsigned char>(key[i])) !=
                    std::tolower(static_cast<unsigned char>(name[i]))) {
                    same = false;
                    break;
                }
            }
            if (same) return value;
        }
    }
    return {};
}

Value remote_undefined() {
    Value result = object_value();
    result.object.emplace_back("type", string_value("undefined"));
    return result;
}

Value remote_string(std::string value) {
    Value result = object_value();
    result.object.emplace_back("type", string_value("string"));
    result.object.emplace_back("value", string_value(std::move(value)));
    return result;
}

Value remote_bool(bool value) {
    Value result = object_value();
    result.object.emplace_back("type", string_value("boolean"));
    result.object.emplace_back("value", bool_value(value));
    return result;
}

Value remote_null() {
    Value result = object_value();
    result.object.emplace_back("type", string_value("object"));
    result.object.emplace_back("subtype", string_value("null"));
    result.object.emplace_back("value", Value{});
    return result;
}

std::string sha1(std::string_view input) {
    std::uint32_t h0 = 0x67452301;
    std::uint32_t h1 = 0xEFCDAB89;
    std::uint32_t h2 = 0x98BADCFE;
    std::uint32_t h3 = 0x10325476;
    std::uint32_t h4 = 0xC3D2E1F0;
    std::string data(input);
    const std::uint64_t bit_length = static_cast<std::uint64_t>(data.size()) * 8;
    data.push_back(static_cast<char>(0x80));
    while ((data.size() % 64) != 56) data.push_back('\0');
    for (int shift = 56; shift >= 0; shift -= 8) {
        data.push_back(static_cast<char>((bit_length >> shift) & 0xff));
    }
    for (std::size_t offset = 0; offset < data.size(); offset += 64) {
        std::uint32_t words[80]{};
        for (int i = 0; i < 16; ++i) {
            const auto at = offset + static_cast<std::size_t>(i) * 4;
            words[i] = (static_cast<std::uint32_t>(
                            static_cast<unsigned char>(data[at])) << 24) |
                       (static_cast<std::uint32_t>(
                            static_cast<unsigned char>(data[at + 1])) << 16) |
                       (static_cast<std::uint32_t>(
                            static_cast<unsigned char>(data[at + 2])) << 8) |
                       static_cast<std::uint32_t>(
                           static_cast<unsigned char>(data[at + 3]));
        }
        for (int i = 16; i < 80; ++i) {
            const std::uint32_t value = words[i - 3] ^ words[i - 8] ^
                                        words[i - 14] ^ words[i - 16];
            words[i] = (value << 1) | (value >> 31);
        }
        std::uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; ++i) {
            std::uint32_t f = 0;
            std::uint32_t k = 0;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }
            const std::uint32_t rotated = (a << 5) | (a >> 27);
            const std::uint32_t next = rotated + f + e + k + words[i];
            e = d;
            d = c;
            c = (b << 30) | (b >> 2);
            b = a;
            a = next;
        }
        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
    }
    std::string result(20, '\0');
    const std::uint32_t words[] = {h0, h1, h2, h3, h4};
    for (int i = 0; i < 5; ++i) {
        result[i * 4] = static_cast<char>(words[i] >> 24);
        result[i * 4 + 1] = static_cast<char>(words[i] >> 16);
        result[i * 4 + 2] = static_cast<char>(words[i] >> 8);
        result[i * 4 + 3] = static_cast<char>(words[i]);
    }
    return result;
}

std::string base64(std::string_view input) {
    static constexpr char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    std::uint32_t accumulator = 0;
    int bits = 0;
    for (const unsigned char c : input) {
        accumulator = (accumulator << 8) | c;
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            result.push_back(table[(accumulator >> bits) & 0x3f]);
        }
    }
    if (bits != 0) result.push_back(table[(accumulator << (6 - bits)) & 0x3f]);
    while (result.size() % 4 != 0) result.push_back('=');
    return result;
}

#if !defined(_WIN32)
bool send_all(int fd, std::string_view data) {
    std::size_t offset = 0;
    while (offset < data.size()) {
        const ssize_t count = ::send(fd, data.data() + offset,
                                     data.size() - offset, MSG_NOSIGNAL);
        if (count <= 0) return false;
        offset += static_cast<std::size_t>(count);
    }
    return true;
}

bool read_exact(int fd, void* buffer, std::size_t size) {
    auto* bytes = static_cast<char*>(buffer);
    std::size_t offset = 0;
    while (offset < size) {
        const ssize_t count = ::recv(fd, bytes + offset, size - offset, 0);
        if (count <= 0) return false;
        offset += static_cast<std::size_t>(count);
    }
    return true;
}

bool write_frame(int fd, std::uint8_t opcode, std::string_view payload) {
    std::string frame;
    frame.push_back(static_cast<char>(0x80 | (opcode & 0x0f)));
    if (payload.size() < 126) {
        frame.push_back(static_cast<char>(payload.size()));
    } else if (payload.size() <= 0xffff) {
        frame.push_back(126);
        frame.push_back(static_cast<char>((payload.size() >> 8) & 0xff));
        frame.push_back(static_cast<char>(payload.size() & 0xff));
    } else {
        frame.push_back(127);
        for (int shift = 56; shift >= 0; shift -= 8) {
            frame.push_back(static_cast<char>((payload.size() >> shift) & 0xff));
        }
    }
    frame.append(payload);
    return send_all(fd, frame);
}

bool read_frame(int fd, std::uint8_t& opcode, std::string& payload) {
    unsigned char header[2];
    if (!read_exact(fd, header, sizeof header)) return false;
    opcode = header[0] & 0x0f;
    const bool masked = (header[1] & 0x80) != 0;
    std::uint64_t length = header[1] & 0x7f;
    if (length == 126) {
        unsigned char extended[2];
        if (!read_exact(fd, extended, sizeof extended)) return false;
        length = (static_cast<std::uint64_t>(extended[0]) << 8) | extended[1];
    } else if (length == 127) {
        unsigned char extended[8];
        if (!read_exact(fd, extended, sizeof extended)) return false;
        length = 0;
        for (const auto byte : extended) length = (length << 8) | byte;
    }
    if (length > 64u * 1024u * 1024u) return false;
    unsigned char mask[4]{};
    if (masked && !read_exact(fd, mask, sizeof mask)) return false;
    payload.resize(static_cast<std::size_t>(length));
    if (!payload.empty() && !read_exact(fd, payload.data(), payload.size())) return false;
    if (masked) {
        for (std::size_t i = 0; i < payload.size(); ++i) {
            payload[i] = static_cast<char>(
                static_cast<unsigned char>(payload[i]) ^ mask[i % 4]);
        }
    }
    return true;
}
#endif

}  // namespace

struct CdpServer::Impl {
    explicit Impl(Browser& browser_ref) : browser(&browser_ref) {
        session = browser->create_session();
        session->load_html("<html><head></head><body></body></html>",
                           "about:blank");
    }

    Browser* browser;
    std::shared_ptr<Session> session;
    std::map<std::string, std::shared_ptr<Element>> objects;
    std::map<int, std::shared_ptr<Element>> nodes;
    int next_node = 1;
    int next_object = 1;
    std::string attached_session = "prowsetk-cdp-session";
    std::string target_id = "prowsetk-page";

    std::string object_id(const std::shared_ptr<Element>& element) {
        const std::string id = "prowsetk-object-" + std::to_string(next_object++);
        objects[id] = element;
        return id;
    }

    std::shared_ptr<Element> object(std::string_view id) const {
        const auto it = objects.find(std::string(id));
        return it == objects.end() ? nullptr : it->second;
    }

    int node_id(const std::shared_ptr<Element>& element) {
        for (const auto& [id, existing] : nodes) {
            if (existing != nullptr && existing->node() == element->node()) return id;
        }
        const int id = next_node++;
        nodes.emplace(id, element);
        return id;
    }

    std::shared_ptr<Element> node(int id) const {
        const auto it = nodes.find(id);
        return it == nodes.end() ? nullptr : it->second;
    }
};

namespace {

Value cdp_result(std::int64_t id, Value result, std::string_view session_id = {}) {
    Value response = object_value();
    response.object.emplace_back("id", number_value(static_cast<double>(id)));
    response.object.emplace_back("result", std::move(result));
    if (!session_id.empty()) {
        response.object.emplace_back("sessionId", string_value(std::string(session_id)));
    }
    return response;
}

Value cdp_event(std::string method, Value params,
                std::string_view session_id = {}) {
    Value event = object_value();
    event.object.emplace_back("method", string_value(std::move(method)));
    event.object.emplace_back("params", std::move(params));
    if (!session_id.empty()) {
        event.object.emplace_back("sessionId", string_value(std::string(session_id)));
    }
    return event;
}

Value frame_tree(const CdpServer::Impl& impl) {
    Value frame = object_value();
    frame.object.emplace_back("id", string_value(impl.target_id));
    frame.object.emplace_back("loaderId", string_value("prowsetk-loader"));
    frame.object.emplace_back("url", string_value(impl.session->current_url()));
    frame.object.emplace_back("domainAndRegistry", string_value(""));
    frame.object.emplace_back("securityOrigin", string_value(""));
    frame.object.emplace_back("mimeType", string_value("text/html"));
    Value result = object_value();
    result.object.emplace_back("frame", std::move(frame));
    return result;
}

std::string extract_quoted(std::string_view expression, std::size_t start) {
    const std::size_t quote = expression.find_first_of("'\"", start);
    if (quote == std::string_view::npos) return {};
    const char delimiter = expression[quote];
    const std::size_t end = expression.find(delimiter, quote + 1);
    return end == std::string_view::npos ?
               std::string{} :
               std::string(expression.substr(quote + 1, end - quote - 1));
}

Value element_description(CdpServer::Impl& impl,
                          const std::shared_ptr<Element>& element) {
    Value result = object_value();
    result.object.emplace_back("type", string_value("object"));
    result.object.emplace_back("subtype", string_value("node"));
    result.object.emplace_back("className", string_value("Element"));
    result.object.emplace_back("description", string_value(element->tag_name()));
    result.object.emplace_back("objectId", string_value(impl.object_id(element)));
    return result;
}

Value value_for_expression(CdpServer::Impl& impl, std::string expression) {
    while (!expression.empty() &&
           std::isspace(static_cast<unsigned char>(expression.front()))) {
        expression.erase(expression.begin());
    }
    while (!expression.empty() &&
           std::isspace(static_cast<unsigned char>(expression.back()))) {
        expression.pop_back();
    }
    if (expression.rfind("return ", 0) == 0) expression.erase(0, 7);
    if (expression == "undefined") return remote_undefined();
    if (expression == "null") return remote_null();
    if (expression == "true") return remote_bool(true);
    if (expression == "false") return remote_bool(false);
    if (expression == "document.title") {
        return remote_string(impl.session->document() ?
                                 impl.session->document()->title() : std::string{});
    }
    if (expression == "location.href" || expression == "document.URL") {
        return remote_string(impl.session->current_url());
    }
    if (expression == "document.readyState") return remote_string("complete");
    if (expression == "document.body.innerText" ||
        expression == "document.body.textContent") {
        const auto body = impl.session->document() ?
                              impl.session->document()->query_selector("body") : nullptr;
        return remote_string(body ? body->text() : std::string{});
    }
    if (expression == "document.documentElement.outerHTML" ||
        expression == "document.documentElement.innerHTML") {
        return remote_string(impl.session->document() ?
                                 impl.session->document()->html() : std::string{});
    }

    const std::size_t query = expression.find("document.querySelector(");
    if (query != std::string::npos) {
        const std::string selector = extract_quoted(expression, query);
        auto element = impl.session->document() ?
                           impl.session->document()->query_selector(selector) : nullptr;
        if (element == nullptr) return remote_null();
        const std::size_t close = expression.find(')', query);
        if (close == std::string::npos || close + 1 >= expression.size()) {
            return element_description(impl, element);
        }
        const std::string suffix = expression.substr(close + 1);
        if (suffix.find(".textContent") != std::string::npos ||
            suffix.find(".innerText") != std::string::npos) {
            return remote_string(element->text());
        }
        if (suffix.find(".value") != std::string::npos) {
            return remote_string(element->value());
        }
        if (suffix.find(".click()") != std::string::npos) {
            element->set_attribute("data-prowsetk-clicked", "true");
            return remote_undefined();
        }
        if (suffix.find(".getAttribute(") != std::string::npos) {
            return remote_string(element->attribute(
                extract_quoted(suffix, suffix.find(".getAttribute("))));
        }
        return element_description(impl, element);
    }

    if (expression.find("JSON.stringify(") == 0) {
        return remote_string("{}");
    }
    if (impl.browser->config().javascript) {
        try {
            return remote_string(impl.session->evaluate_js(expression));
        } catch (...) {
            return remote_undefined();
        }
    }
    return remote_undefined();
}

Value call_function(CdpServer::Impl& impl, const Value& params) {
    const Value* object_id = params.find("objectId");
    auto element = object_id != nullptr && object_id->is_string() ?
                       impl.object(object_id->string) : nullptr;
    const Value* declaration = params.find("functionDeclaration");
    const std::string function = declaration != nullptr && declaration->is_string() ?
                                     declaration->string : std::string{};
    if (element == nullptr) return remote_undefined();
    if (function.find("getBoundingClientRect") != std::string::npos) {
        Value rect = object_value();
        rect.object.emplace_back("x", number_value(0));
        rect.object.emplace_back("y", number_value(0));
        rect.object.emplace_back("width", number_value(0));
        rect.object.emplace_back("height", number_value(0));
        Value result = object_value();
        result.object.emplace_back("type", string_value("object"));
        result.object.emplace_back("value", std::move(rect));
        return result;
    }
    if (function.find("textContent") != std::string::npos ||
        function.find("innerText") != std::string::npos) {
        return remote_string(element->text());
    }
    if (function.find(".value") != std::string::npos ||
        function.find("value;") != std::string::npos) {
        return remote_string(element->value());
    }
    if (function.find("getAttribute") != std::string::npos) {
        return remote_string(element->attribute(
            extract_quoted(function, function.find("getAttribute"))));
    }
    if (function.find("setAttribute") != std::string::npos) {
        element->set_attribute("data-prowsetk-value", "true");
        return remote_undefined();
    }
    if (function.find("click") != std::string::npos) {
        element->set_attribute("data-prowsetk-clicked", "true");
    }
    return remote_undefined();
}

Value dom_node(CdpServer::Impl& impl, const std::shared_ptr<Element>& element,
               bool with_children) {
    Value node = object_value();
    const int node_id = impl.node_id(element);
    node.object.emplace_back("nodeId", number_value(node_id));
    node.object.emplace_back("backendNodeId", number_value(node_id));
    node.object.emplace_back("nodeType", number_value(1));
    node.object.emplace_back("nodeName", string_value(element->tag_name()));
    node.object.emplace_back("localName", string_value(element->tag_name()));
    node.object.emplace_back("nodeValue", string_value(""));
    node.object.emplace_back("childNodeCount",
                             number_value(static_cast<double>(element->children().size())));
    Value attributes = array_value();
    for (const auto& attribute : element->attributes()) {
        attributes.array.push_back(string_value(attribute.name));
        attributes.array.push_back(string_value(attribute.value));
    }
    node.object.emplace_back("attributes", std::move(attributes));
    if (with_children) {
        Value children = array_value();
        for (const auto& child : element->children()) {
            children.array.push_back(dom_node(impl, child, true));
        }
        node.object.emplace_back("children", std::move(children));
    }
    return node;
}

Value dispatch(CdpServer::Impl& impl, const Value& message,
               std::string_view session_id,
               std::vector<Value>& events) {
    const Value* id = message.find("id");
    const std::int64_t command_id = id != nullptr && id->is_number() ?
        static_cast<std::int64_t>(id->number) : 0;
    const Value* method_value = message.find("method");
    const std::string method = method_value != nullptr && method_value->is_string() ?
                                   method_value->string : std::string{};
    const Value* params = message.find("params");
    const Value empty_params = object_value();
    const Value& arguments = params != nullptr ? *params : empty_params;

    if (method == "Target.attachToTarget") {
        Value result = object_value();
        result.object.emplace_back("sessionId", string_value(impl.attached_session));
        return cdp_result(command_id, std::move(result));
    }
    if (method == "Target.sendMessageToTarget") {
        const Value* nested = arguments.find("message");
        if (nested != nullptr && nested->is_string()) {
            Value inner;
            if (Parser(nested->string).parse(inner)) {
                (void)dispatch(impl, inner, impl.attached_session, events);
            }
        }
        return cdp_result(command_id, object_value(), session_id);
    }
    if (method == "Target.getTargets") {
        Value target = object_value();
        target.object.emplace_back("targetId", string_value(impl.target_id));
        target.object.emplace_back("type", string_value("page"));
        target.object.emplace_back("title", string_value(
            impl.session->document() ? impl.session->document()->title() : std::string{}));
        target.object.emplace_back("url", string_value(impl.session->current_url()));
        target.object.emplace_back("attached", bool_value(true));
        Value result = object_value();
        Value infos = array_value();
        infos.array.push_back(std::move(target));
        result.object.emplace_back("targetInfos", std::move(infos));
        return cdp_result(command_id, std::move(result), session_id);
    }
    if (method == "Target.getBrowserContexts") {
        Value result = object_value();
        result.object.emplace_back("browserContextIds", array_value());
        return cdp_result(command_id, std::move(result));
    }
    if (method == "Browser.getVersion") {
        Value result = object_value();
        result.object.emplace_back("protocolVersion", string_value("1.3"));
        result.object.emplace_back("product",
                                   string_value(std::string("ProwseTk/") + version()));
        result.object.emplace_back("revision", string_value(version()));
        result.object.emplace_back("userAgent",
                                   string_value(std::string("ProwseTk/") + version()));
        result.object.emplace_back("jsVersion", string_value("QuickJS"));
        return cdp_result(command_id, std::move(result));
    }
    if (method == "Page.navigate") {
        const Value* url = arguments.find("url");
        if (url != nullptr && url->is_string()) {
            if (url->string.rfind("data:text/html", 0) == 0) {
                impl.session->load_html(data_html(url->string), url->string);
            } else if (url->string == "about:blank") {
                impl.session->load_html(
                    "<html><head></head><body></body></html>", "about:blank");
            } else {
                impl.session->navigate(url->string);
            }
        }
        Value started = object_value();
        started.object.emplace_back("frameId", string_value(impl.target_id));
        events.push_back(cdp_event("Page.frameStartedLoading", std::move(started),
                                   session_id));
        Value navigated = frame_tree(impl).find("frame") != nullptr ?
                              *frame_tree(impl).find("frame") : object_value();
        Value frame_event = object_value();
        frame_event.object.emplace_back("frame", std::move(navigated));
        events.push_back(cdp_event("Page.frameNavigated", std::move(frame_event),
                                   session_id));
        Value loaded = object_value();
        loaded.object.emplace_back("timestamp", number_value(0));
        events.push_back(cdp_event("Page.loadEventFired", std::move(loaded),
                                   session_id));
        Value result = object_value();
        result.object.emplace_back("frameId", string_value(impl.target_id));
        result.object.emplace_back("loaderId", string_value("prowsetk-loader"));
        return cdp_result(command_id, std::move(result), session_id);
    }
    if (method == "Page.reload") {
        if (!impl.session->current_url().empty() &&
            impl.session->current_url() != "about:blank") {
            impl.session->navigate(impl.session->current_url());
        }
        return cdp_result(command_id, object_value(), session_id);
    }
    if (method == "Page.getFrameTree") {
        return cdp_result(command_id, frame_tree(impl), session_id);
    }
    if (method == "Page.getNavigationHistory") {
        Value result = object_value();
        result.object.emplace_back("currentIndex", number_value(0));
        result.object.emplace_back("entries", array_value());
        return cdp_result(command_id, std::move(result), session_id);
    }
    if (method == "Runtime.evaluate") {
        const Value* expression = arguments.find("expression");
        const std::string script = expression != nullptr && expression->is_string() ?
                                       expression->string : std::string{};
        Value result = object_value();
        result.object.emplace_back("result", value_for_expression(impl, script));
        return cdp_result(command_id, std::move(result), session_id);
    }
    if (method == "Runtime.callFunctionOn") {
        Value result = object_value();
        result.object.emplace_back("result", call_function(impl, arguments));
        return cdp_result(command_id, std::move(result), session_id);
    }
    if (method == "DOM.getDocument") {
        Value result = object_value();
        auto root = impl.session->document() ? impl.session->document()->root() : nullptr;
        result.object.emplace_back("root", root ? dom_node(impl, root, true) :
                                                  object_value());
        return cdp_result(command_id, std::move(result), session_id);
    }
    if (method == "DOM.querySelector" || method == "DOM.querySelectorAll") {
        const Value* node_id = arguments.find("nodeId");
        const Value* selector = arguments.find("selector");
        const auto root = node_id != nullptr && node_id->is_number() ?
                              impl.node(static_cast<int>(node_id->number)) :
                              (impl.session->document() ? impl.session->document()->root() :
                                                          nullptr);
        const std::string css = selector != nullptr && selector->is_string() ?
                                    selector->string : std::string{};
        std::vector<std::shared_ptr<Element>> matches;
        if (root != nullptr) {
            matches = root->query_selector_all(css);
        }
        Value result = object_value();
        if (method == "DOM.querySelector") {
            result.object.emplace_back("nodeId",
                number_value(matches.empty() ? 0 : impl.node_id(matches.front())));
        } else {
            Value ids = array_value();
            for (const auto& match : matches) {
                ids.array.push_back(number_value(impl.node_id(match)));
            }
            result.object.emplace_back("nodeIds", std::move(ids));
        }
        return cdp_result(command_id, std::move(result), session_id);
    }
    if (method == "DOM.getOuterHTML") {
        const Value* node_id = arguments.find("nodeId");
        const auto element = node_id != nullptr && node_id->is_number() ?
                                 impl.node(static_cast<int>(node_id->number)) : nullptr;
        Value result = object_value();
        result.object.emplace_back("outerHTML",
            string_value(element ? element->outer_html() : std::string{}));
        return cdp_result(command_id, std::move(result), session_id);
    }
    if (method == "DOM.getAttributes") {
        const Value* node_id = arguments.find("nodeId");
        const auto element = node_id != nullptr && node_id->is_number() ?
                                 impl.node(static_cast<int>(node_id->number)) : nullptr;
        Value result = object_value();
        Value attributes = array_value();
        if (element != nullptr) {
            for (const auto& attribute : element->attributes()) {
                attributes.array.push_back(string_value(attribute.name));
                attributes.array.push_back(string_value(attribute.value));
            }
        }
        result.object.emplace_back("attributes", std::move(attributes));
        return cdp_result(command_id, std::move(result), session_id);
    }
    if (method == "Page.getLayoutMetrics") {
        Value result = object_value();
        Value layout = object_value();
        layout.object.emplace_back("x", number_value(0));
        layout.object.emplace_back("y", number_value(0));
        layout.object.emplace_back("width", number_value(1280));
        layout.object.emplace_back("height", number_value(720));
        result.object.emplace_back("contentSize", layout);
        result.object.emplace_back("layoutViewport", layout);
        result.object.emplace_back("visualViewport", layout);
        return cdp_result(command_id, std::move(result), session_id);
    }
    if (method == "Page.captureScreenshot") {
        // A valid 1x1 transparent PNG keeps Playwright's screenshot API
        // deterministic without pretending that Flatworm renders pixels.
        static constexpr char png[] =
            "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk"
            "YAAAAAYAAjCB0C8AAAAASUVORK5CYII=";
        Value result = object_value();
        result.object.emplace_back("data", string_value(png));
        return cdp_result(command_id, std::move(result), session_id);
    }
    if (method == "Target.closeTarget") {
        return cdp_result(command_id, bool_value(true), session_id);
    }
    if (method == "Browser.close" || method == "Page.close") {
        return cdp_result(command_id, object_value(), session_id);
    }
    if (method == "Runtime.enable") {
        Value params_event = object_value();
        Value context = object_value();
        context.object.emplace_back("id", number_value(1));
        context.object.emplace_back("origin", string_value(""));
        context.object.emplace_back("name", string_value(""));
        params_event.object.emplace_back("context", std::move(context));
        events.push_back(cdp_event("Runtime.executionContextCreated",
                                   std::move(params_event), session_id));
    }
    // Most enable/configuration commands are intentionally no-ops. Returning a
    // successful empty result is the CDP-compatible behavior for a headless
    // engine that does not implement that optional domain.
    return cdp_result(command_id, object_value(), session_id);
}

}  // namespace

CdpServer::CdpServer(Browser& browser) : impl_(std::make_unique<Impl>(browser)) {}
CdpServer::~CdpServer() = default;

WebResponse CdpServer::handle_http(const WebRequest& request) {
    const std::string host = request_header(request, "Host").empty() ?
                                 "127.0.0.1" : request_header(request, "Host");
    const std::string ws_base = "ws://" + host;
    const std::string path = request.path;
    if (request.method != "GET") return WebResponse::method_not_allowed();
    if (path == "/json/version") {
        Value body = object_value();
        body.object.emplace_back("Browser",
                                 string_value(std::string("ProwseTk/") + version()));
        body.object.emplace_back("Protocol-Version", string_value("1.3"));
        body.object.emplace_back("User-Agent",
                                 string_value(std::string("ProwseTk/") + version()));
        body.object.emplace_back("V8-Version", string_value("QuickJS"));
        body.object.emplace_back("webSocketDebuggerUrl",
                                 string_value(ws_base + "/devtools/browser/prowsetk"));
        return WebResponse::json(200, serialize_value(body));
    }
    if (path == "/json" || path == "/json/" || path == "/json/list") {
        Value target = object_value();
        target.object.emplace_back("description", string_value(""));
        target.object.emplace_back("devtoolsFrontendUrl", string_value(""));
        target.object.emplace_back("id", string_value(impl_->target_id));
        target.object.emplace_back("title", string_value(
            impl_->session->document() ? impl_->session->document()->title() : std::string{}));
        target.object.emplace_back("type", string_value("page"));
        target.object.emplace_back("url", string_value(impl_->session->current_url()));
        target.object.emplace_back("webSocketDebuggerUrl",
            string_value(ws_base + "/devtools/page/" + impl_->target_id));
        Value list = array_value();
        list.array.push_back(std::move(target));
        return WebResponse::json(200, serialize_value(list));
    }
    if (path == "/json/close/" + impl_->target_id) {
        return WebResponse::text(200, "Target is closing");
    }
    return WebResponse::not_found();
}

void CdpServer::handle_websocket(int fd, const WebRequest& request) {
#if defined(_WIN32)
    (void)fd;
    (void)request;
#else
    const std::string key = request_header(request, "Sec-WebSocket-Key");
    if (key.empty()) {
        send_all(fd, "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n"
                     "Content-Length: 0\r\n\r\n");
        return;
    }
    const std::string accept = base64(sha1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"));
    const std::string handshake =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";
    if (!send_all(fd, handshake)) return;

    std::uint8_t opcode = 0;
    std::string payload;
    while (read_frame(fd, opcode, payload)) {
        if (opcode == 0x8) {
            write_frame(fd, 0x8, {});
            return;
        }
        if (opcode == 0x9) {
            if (!write_frame(fd, 0xA, payload)) return;
            continue;
        }
        if (opcode != 0x1) continue;
        Value message;
        if (!Parser(payload).parse(message)) continue;
        std::vector<Value> events;
        const Value* session_id = message.find("sessionId");
        const std::string session =
            session_id != nullptr && session_id->is_string() ?
                session_id->string : std::string{};
        const Value response = dispatch(*impl_, message, session, events);
        for (const auto& event : events) {
            if (!write_frame(fd, 0x1, serialize_value(event))) return;
        }
        if (!write_frame(fd, 0x1, serialize_value(response))) return;
    }
#endif
}

}  // namespace prowsetk
