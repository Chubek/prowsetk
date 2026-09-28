#include "flatworm/html_syntax.hpp"
#include <algorithm>
namespace prowsetk::flatworm {
std::string to_lower(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](char c) {
        return static_cast<char>((c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c));
    });
    return result;
}

bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

bool is_void_element(std::string_view name) {
    static const char* const voids[] = {
        "area", "base", "br",  "col",   "embed", "hr",    "img", "input",
        "link", "meta", "param", "source", "track", "wbr"};
    for (const char* element : voids) {
        if (name == element) {
            return true;
        }
    }
    return false;
}

bool is_raw_text_element(std::string_view name) {
    return name == "script" || name == "style";
}

bool is_rcdata_element(std::string_view name) {
    return name == "textarea" || name == "title";
}
}
