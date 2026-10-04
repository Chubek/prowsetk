#include "json.hpp"

#include <set>

namespace flatworm::rpc {
namespace {

bool digit(char c) { return c >= '0' && c <= '9'; }

std::uint32_t utf8_scalar(std::string_view bytes, std::size_t& position) {
    const auto first = static_cast<unsigned char>(bytes[position++]);
    if (first < 0x80) return first;
    unsigned count;
    std::uint32_t value;
    std::uint32_t minimum;
    if (first >= 0xc2 && first <= 0xdf) { count = 1; value = first & 0x1f; minimum = 0x80; }
    else if (first >= 0xe0 && first <= 0xef) { count = 2; value = first & 0x0f; minimum = 0x800; }
    else if (first >= 0xf0 && first <= 0xf4) { count = 3; value = first & 0x07; minimum = 0x10000; }
    else invalid();
    if (count > bytes.size() - position) invalid();
    for (unsigned i = 0; i < count; ++i) {
        const auto next = static_cast<unsigned char>(bytes[position++]);
        if ((next & 0xc0) != 0x80) invalid();
        value = (value << 6) | (next & 0x3f);
    }
    if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) invalid();
    return value;
}

void append_scalar(std::string& out, std::uint32_t value) {
    if (value < 0x80) out.push_back(static_cast<char>(value));
    else if (value < 0x800) {
        out.push_back(static_cast<char>(0xc0 | (value >> 6)));
        out.push_back(static_cast<char>(0x80 | (value & 0x3f)));
    } else if (value < 0x10000) {
        out.push_back(static_cast<char>(0xe0 | (value >> 12)));
        out.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (value & 0x3f)));
    } else {
        out.push_back(static_cast<char>(0xf0 | (value >> 18)));
        out.push_back(static_cast<char>(0x80 | ((value >> 12) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (value & 0x3f)));
    }
}

class Parser {
public:
    explicit Parser(std::string_view source) : source_(source) {
        if (source.size() > FLATWORM_MODULE_MAX_VALUE_BYTES) resource_limit();
    }
    Json parse() {
        auto value = read_value(0);
        whitespace();
        if (position_ != source_.size()) invalid();
        return value;
    }

private:
    char peek() const { return position_ < source_.size() ? source_[position_] : '\0'; }
    char take() { if (position_ == source_.size()) invalid(); return source_[position_++]; }
    void expect(char c) { if (take() != c) invalid(); }
    void whitespace() {
        while (peek() == ' ' || peek() == '\t' || peek() == '\r' || peek() == '\n') ++position_;
    }
    void literal(std::string_view word) {
        if (source_.substr(position_, word.size()) != word) invalid();
        position_ += word.size();
    }
    std::uint32_t hex4() {
        std::uint32_t value = 0;
        for (unsigned i = 0; i < 4; ++i) {
            const char c = take();
            unsigned part;
            if (digit(c)) part = static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') part = static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') part = static_cast<unsigned>(c - 'A' + 10);
            else invalid();
            value = (value << 4) | part;
        }
        return value;
    }
    std::string string() {
        expect('"');
        std::string out;
        while (peek() != '"') {
            const auto start = position_;
            const auto c = static_cast<unsigned char>(take());
            if (c < 0x20) invalid();
            if (c >= 0x80) {
                position_ = start;
                (void)utf8_scalar(source_, position_);
                out.append(source_.substr(start, position_ - start));
            } else if (c != '\\') out.push_back(static_cast<char>(c));
            else {
                switch (take()) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u': {
                        auto value = hex4();
                        if (value >= 0xd800 && value <= 0xdbff) {
                            expect('\\'); expect('u');
                            const auto low = hex4();
                            if (low < 0xdc00 || low > 0xdfff) invalid();
                            value = 0x10000 + ((value - 0xd800) << 10) + (low - 0xdc00);
                        } else if (value >= 0xdc00 && value <= 0xdfff) invalid();
                        append_scalar(out, value);
                        break;
                    }
                    default: invalid();
                }
            }
        }
        expect('"');
        return out;
    }
    Json number() {
        const auto start = position_;
        if (peek() == '-') ++position_;
        if (peek() == '0') ++position_;
        else {
            if (peek() < '1' || peek() > '9') invalid();
            while (digit(peek())) ++position_;
        }
        if (peek() == '.') {
            ++position_;
            if (!digit(peek())) invalid();
            while (digit(peek())) ++position_;
        }
        if (peek() == 'e' || peek() == 'E') {
            ++position_;
            if (peek() == '+' || peek() == '-') ++position_;
            if (!digit(peek())) invalid();
            while (digit(peek())) ++position_;
        }
        return Json::parsed_number(source_.substr(start, position_ - start));
    }
    Json read_value(std::size_t depth) {
        if (depth > max_json_depth || ++values_ > max_json_values) resource_limit();
        whitespace();
        switch (peek()) {
            case 'n': literal("null"); return {};
            case 't': literal("true"); return Json::boolean_value(true);
            case 'f': literal("false"); return Json::boolean_value(false);
            case '"': return Json::string_value(string());
            case '[': {
                ++position_;
                auto result = Json::array_value();
                whitespace();
                if (peek() == ']') { ++position_; return result; }
                for (;;) {
                    result.array.push_back(read_value(depth + 1));
                    whitespace();
                    const char separator = take();
                    if (separator == ']') return result;
                    if (separator != ',') invalid();
                }
            }
            case '{': {
                ++position_;
                auto result = Json::object_value();
                std::set<std::string> names;
                whitespace();
                if (peek() == '}') { ++position_; return result; }
                for (;;) {
                    whitespace();
                    auto key = string();
                    if (!names.insert(key).second) invalid();
                    whitespace(); expect(':');
                    result.put(std::move(key), read_value(depth + 1));
                    whitespace();
                    const char separator = take();
                    if (separator == '}') return result;
                    if (separator != ',') invalid();
                }
            }
            default: return number();
        }
    }

    std::string_view source_;
    std::size_t position_ = 0;
    std::size_t values_ = 0;
};

}  // namespace

void validate_utf8(std::string_view bytes) {
    for (std::size_t position = 0; position < bytes.size();) (void)utf8_scalar(bytes, position);
}

Json parse_json(std::string_view source) { return Parser(source).parse(); }

}  // namespace flatworm::rpc
