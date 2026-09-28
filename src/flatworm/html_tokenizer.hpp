#ifndef PROWSETK_FLATWORM_HTML_TOKENIZER_HPP
#define PROWSETK_FLATWORM_HTML_TOKENIZER_HPP
#include "flatworm/html_model.hpp"

namespace prowsetk::flatworm {
// Streaming syntax layer. No node creation, tree traversal, or mutation.
class HtmlTokenizer {
public:
    explicit HtmlTokenizer(std::string_view input) : input_(input) {}
    HtmlToken next();
    void text_element(std::string_view name) { text_element_ = name; }
private:
    std::string_view input_;
    std::size_t position_ = 0;
    std::string text_element_;
};
}
#endif
