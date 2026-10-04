#include "json.hpp"

namespace flatworm::rpc {
namespace {

class Encoder {
public:
    std::string encode(const Json& value) { write(value, 0); return std::move(output_); }

private:
    void append(std::string_view bytes) {
        if (bytes.size() > FLATWORM_MODULE_MAX_VALUE_BYTES - output_.size()) resource_limit();
        output_.append(bytes);
    }
    void quoted(std::string_view text) {
        validate_utf8(text);
        append("\"");
        constexpr char hex[] = "0123456789abcdef";
        for (const auto c : text) {
            const auto byte = static_cast<unsigned char>(c);
            if (c == '"') append("\\\"");
            else if (c == '\\') append("\\\\");
            else if (byte < 0x20) {
                const char escaped[] = {'\\', 'u', '0', '0', hex[byte >> 4], hex[byte & 15]};
                append(std::string_view(escaped, sizeof(escaped)));
            } else append(std::string_view(&c, 1));
        }
        append("\"");
    }
    void write(const Json& value, std::size_t depth) {
        if (depth > max_json_depth || ++values_ > max_json_values) resource_limit();
        switch (value.kind) {
            case Json::Kind::Null: append("null"); break;
            case Json::Kind::Boolean: append(value.boolean ? "true" : "false"); break;
            case Json::Kind::Number: append(value.text); break;
            case Json::Kind::String: quoted(value.text); break;
            case Json::Kind::Array: {
                append("[");
                bool first = true;
                for (const auto& item : value.array) {
                    if (!first) append(",");
                    first = false;
                    write(item, depth + 1);
                }
                append("]");
                break;
            }
            case Json::Kind::Object: {
                append("{");
                bool first = true;
                for (const auto& [key, item] : value.object) {
                    if (!first) append(",");
                    first = false;
                    quoted(key); append(":"); write(item, depth + 1);
                }
                append("}");
                break;
            }
        }
    }

    std::string output_;
    std::size_t values_ = 0;
};

}  // namespace

std::string encode_json(const Json& value) { return Encoder().encode(value); }

}  // namespace flatworm::rpc
