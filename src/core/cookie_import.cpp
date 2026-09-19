#include "prowsetk/cookie_import.hpp"

#include <cctype>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "prowsetk/error.hpp"
#include "prowsetk/storage.hpp"
#include "prowsetk/url.hpp"

namespace prowsetk {
namespace {

struct Json {
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    std::vector<Json> array;
    std::map<std::string, Json> object;

    const Json* member(const char* key) const {
        if (type != Type::Object) {
            return nullptr;
        }
        const auto it = object.find(key);
        return it == object.end() ? nullptr : &it->second;
    }
};

class JsonParser {
public:
    explicit JsonParser(std::string_view input) : input_(input) {}

    Json parse() {
        skip_ws();
        Json value = parse_value();
        skip_ws();
        if (pos_ != input_.size()) {
            fail("unexpected trailing JSON input");
        }
        return value;
    }

private:
    [[noreturn]] void fail(const char* message) const {
        throw Error(ErrorCode::ParseError,
                    std::string("cookie JSON parse error: ") + message);
    }

    bool consume(char expected) {
        skip_ws();
        if (pos_ >= input_.size() || input_[pos_] != expected) {
            return false;
        }
        ++pos_;
        return true;
    }

    void expect(char expected) {
        if (!consume(expected)) {
            fail("unexpected token");
        }
    }

    void skip_ws() {
        while (pos_ < input_.size() &&
               std::isspace(static_cast<unsigned char>(input_[pos_])) != 0) {
            ++pos_;
        }
    }

    Json parse_value() {
        skip_ws();
        if (pos_ >= input_.size()) {
            fail("unexpected end of input");
        }
        const char c = input_[pos_];
        if (c == '"') {
            Json value;
            value.type = Json::Type::String;
            value.string = parse_string();
            return value;
        }
        if (c == '[') {
            return parse_array();
        }
        if (c == '{') {
            return parse_object();
        }
        if (c == 't' || c == 'f') {
            return parse_bool();
        }
        if (c == 'n') {
            return parse_null();
        }
        if (c == '-' || std::isdigit(static_cast<unsigned char>(c)) != 0) {
            return parse_number();
        }
        fail("unexpected token");
    }

    Json parse_null() {
        if (input_.substr(pos_, 4) != "null") {
            fail("invalid null literal");
        }
        pos_ += 4;
        return {};
    }

    Json parse_bool() {
        Json value;
        value.type = Json::Type::Bool;
        if (input_.substr(pos_, 4) == "true") {
            value.boolean = true;
            pos_ += 4;
            return value;
        }
        if (input_.substr(pos_, 5) == "false") {
            value.boolean = false;
            pos_ += 5;
            return value;
        }
        fail("invalid boolean literal");
    }

    Json parse_number() {
        const std::size_t start = pos_;
        if (input_[pos_] == '-') {
            ++pos_;
        }
        if (pos_ >= input_.size()) {
            fail("invalid number");
        }
        if (input_[pos_] == '0') {
            ++pos_;
        } else {
            if (std::isdigit(static_cast<unsigned char>(input_[pos_])) == 0) {
                fail("invalid number");
            }
            while (pos_ < input_.size() &&
                   std::isdigit(static_cast<unsigned char>(input_[pos_])) != 0) {
                ++pos_;
            }
        }
        if (pos_ < input_.size() && input_[pos_] == '.') {
            ++pos_;
            if (pos_ >= input_.size() ||
                std::isdigit(static_cast<unsigned char>(input_[pos_])) == 0) {
                fail("invalid fractional number");
            }
            while (pos_ < input_.size() &&
                   std::isdigit(static_cast<unsigned char>(input_[pos_])) != 0) {
                ++pos_;
            }
        }
        if (pos_ < input_.size() &&
            (input_[pos_] == 'e' || input_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < input_.size() &&
                (input_[pos_] == '+' || input_[pos_] == '-')) {
                ++pos_;
            }
            if (pos_ >= input_.size() ||
                std::isdigit(static_cast<unsigned char>(input_[pos_])) == 0) {
                fail("invalid exponent");
            }
            while (pos_ < input_.size() &&
                   std::isdigit(static_cast<unsigned char>(input_[pos_])) != 0) {
                ++pos_;
            }
        }

        Json value;
        value.type = Json::Type::Number;
        value.number = std::stod(std::string(input_.substr(start, pos_ - start)));
        return value;
    }

    int hex_value(char c) const {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return 10 + c - 'a';
        }
        if (c >= 'A' && c <= 'F') {
            return 10 + c - 'A';
        }
        fail("invalid unicode escape");
    }

    std::string parse_string() {
        expect('"');
        std::string output;
        while (pos_ < input_.size()) {
            char c = input_[pos_++];
            if (c == '"') {
                return output;
            }
            if (static_cast<unsigned char>(c) < 0x20) {
                fail("control character in string");
            }
            if (c != '\\') {
                output.push_back(c);
                continue;
            }
            if (pos_ >= input_.size()) {
                fail("unterminated escape");
            }
            const char escaped = input_[pos_++];
            switch (escaped) {
                case '"':
                case '\\':
                case '/':
                    output.push_back(escaped);
                    break;
                case 'b':
                    output.push_back('\b');
                    break;
                case 'f':
                    output.push_back('\f');
                    break;
                case 'n':
                    output.push_back('\n');
                    break;
                case 'r':
                    output.push_back('\r');
                    break;
                case 't':
                    output.push_back('\t');
                    break;
                case 'u': {
                    if (pos_ + 4 > input_.size()) {
                        fail("short unicode escape");
                    }
                    int codepoint = 0;
                    for (int i = 0; i < 4; ++i) {
                        codepoint = codepoint * 16 + hex_value(input_[pos_++]);
                    }
                    output.push_back(codepoint <= 0x7f
                                         ? static_cast<char>(codepoint)
                                         : '?');
                    break;
                }
                default:
                    fail("invalid escape");
            }
        }
        fail("unterminated string");
    }

    Json parse_array() {
        Json value;
        value.type = Json::Type::Array;
        expect('[');
        skip_ws();
        if (consume(']')) {
            return value;
        }
        while (true) {
            value.array.push_back(parse_value());
            if (consume(']')) {
                return value;
            }
            expect(',');
        }
    }

    Json parse_object() {
        Json value;
        value.type = Json::Type::Object;
        expect('{');
        skip_ws();
        if (consume('}')) {
            return value;
        }
        while (true) {
            skip_ws();
            if (pos_ >= input_.size() || input_[pos_] != '"') {
                fail("expected object key");
            }
            std::string key = parse_string();
            expect(':');
            value.object.emplace(std::move(key), parse_value());
            if (consume('}')) {
                return value;
            }
            expect(',');
        }
    }

    std::string_view input_;
    std::size_t pos_ = 0;
};

std::optional<std::string> json_string(const Json& object, const char* key) {
    const Json* value = object.member(key);
    if (value == nullptr || value->type != Json::Type::String) {
        return std::nullopt;
    }
    return value->string;
}

std::optional<bool> json_bool(const Json& object, const char* key) {
    const Json* value = object.member(key);
    if (value == nullptr || value->type != Json::Type::Bool) {
        return std::nullopt;
    }
    return value->boolean;
}

std::optional<double> json_number(const Json& object, const char* key) {
    const Json* value = object.member(key);
    if (value == nullptr || value->type != Json::Type::Number) {
        return std::nullopt;
    }
    return value->number;
}

std::optional<bool> bool_any(const Json& object, const char* first,
                             const char* second) {
    if (auto value = json_bool(object, first)) {
        return value;
    }
    return json_bool(object, second);
}

std::optional<std::int64_t> expires_from(const Json& object) {
    std::optional<double> value = json_number(object, "expirationDate");
    if (!value) {
        value = json_number(object, "expires");
    }
    if (!value) {
        value = json_number(object, "expires_unix");
    }
    if (!value || *value <= 0.0 || !std::isfinite(*value)) {
        return std::nullopt;
    }
    return static_cast<std::int64_t>(*value);
}

std::string strip_leading_dot(std::string domain) {
    while (!domain.empty() && domain.front() == '.') {
        domain.erase(domain.begin());
    }
    return domain;
}

std::string origin_for_cookie(const Cookie& cookie,
                              const std::optional<std::string>& url,
                              std::string_view default_origin) {
    if (url && !url->empty()) {
        return *url;
    }
    if (!cookie.domain.empty()) {
        return std::string(cookie.secure ? "https://" : "http://") +
               strip_leading_dot(cookie.domain) +
               (cookie.path.empty() ? "/" : cookie.path);
    }
    return std::string(default_origin);
}

CookieImportResult import_cookie_array(CookieJar& jar, const std::vector<Json>& cookies,
                                       std::string_view default_origin) {
    CookieImportResult result;
    for (const Json& item : cookies) {
        if (item.type != Json::Type::Object) {
            ++result.skipped;
            result.warnings.push_back("skipped non-object cookie entry");
            continue;
        }

        auto name = json_string(item, "name");
        auto value = json_string(item, "value");
        if (!name || name->empty() || !value) {
            ++result.skipped;
            result.warnings.push_back("skipped cookie missing name or value");
            continue;
        }

        Cookie cookie;
        cookie.name = std::move(*name);
        cookie.value = std::move(*value);
        cookie.domain = json_string(item, "domain").value_or("");
        cookie.path = json_string(item, "path").value_or("/");
        cookie.secure = json_bool(item, "secure").value_or(false);
        cookie.http_only = bool_any(item, "httpOnly", "http_only").value_or(false);
        cookie.same_site =
            json_string(item, "sameSite")
                .value_or(json_string(item, "same_site").value_or(""));
        cookie.expires_unix = expires_from(item);
        if (auto host_only = bool_any(item, "hostOnly", "host_only")) {
            cookie.host_only = *host_only;
        } else {
            cookie.host_only = cookie.domain.empty() ||
                               (!cookie.domain.empty() && cookie.domain.front() != '.');
        }

        const std::string origin =
            origin_for_cookie(cookie, json_string(item, "url"), default_origin);
        if (origin.empty()) {
            ++result.skipped;
            result.warnings.push_back("skipped cookie without domain or URL");
            continue;
        }

        jar.set(parse_url(origin), std::move(cookie));
        ++result.imported;
    }
    return result;
}

}  // namespace

CookieImportResult import_cookies_json(CookieJar& jar, std::string_view contents,
                                       std::string_view default_origin) {
    const Json root = JsonParser(contents).parse();
    if (root.type == Json::Type::Array) {
        return import_cookie_array(jar, root.array, default_origin);
    }
    if (root.type == Json::Type::Object) {
        const Json* cookies = root.member("cookies");
        if (cookies != nullptr && cookies->type == Json::Type::Array) {
            return import_cookie_array(jar, cookies->array, default_origin);
        }
    }
    throw Error(ErrorCode::ParseError,
                "cookie JSON must be an array or an object with a cookies array");
}

CookieImportResult import_cookies_json_file(CookieJar& jar,
                                            const std::filesystem::path& path,
                                            std::string_view default_origin) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw Error(ErrorCode::IoError,
                    "cannot open cookie JSON file: " + path.string());
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return import_cookies_json(jar, buffer.str(), default_origin);
}

}  // namespace prowsetk
