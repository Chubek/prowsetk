#ifndef PROWSETK_FLATWORM_HTML_MODEL_HPP
#define PROWSETK_FLATWORM_HTML_MODEL_HPP
#include <string>
#include <string_view>
#include <vector>

namespace prowsetk::flatworm {
// Value-only syntax records shared with the DOM; no node ownership or callbacks.
struct Attribute {
    std::string name;
    std::string value;
};
enum class HtmlTokenKind { End, Text, Comment, Doctype, StartTag, EndTag };
struct HtmlToken {
    HtmlTokenKind kind = HtmlTokenKind::End;
    std::string data;
    std::vector<Attribute> attributes;
};
}
#endif
