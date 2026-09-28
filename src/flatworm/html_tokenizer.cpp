#include "flatworm/html_tokenizer.hpp"
#include "flatworm/html_syntax.hpp"
#include <unordered_set>

namespace prowsetk::flatworm {
namespace {
bool ascii_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
}
HtmlToken HtmlTokenizer::next() {
    const auto html = input_;
    auto& i = position_;
    if (i >= html.size()) return {};
    if (!text_element_.empty()) {
        const auto begin = i;
        auto end = html.size();
        for (auto candidate = html.find('<', i); candidate != std::string_view::npos;
             candidate = html.find('<', candidate + 1)) {
            const auto after = candidate + 2 + text_element_.size();
            if (after < html.size() && html[candidate + 1] == '/' &&
                to_lower(html.substr(candidate + 2, text_element_.size())) == text_element_ &&
                (is_space(html[after]) || html[after] == '/' || html[after] == '>')) {
                end = candidate;
                break;
            }
        }
        i = end;
        const auto raw = html.substr(begin, end - begin);
        auto data = is_rcdata_element(text_element_) ? decode_entities(raw) : std::string(raw);
        text_element_.clear();
        if (!data.empty()) return {HtmlTokenKind::Text, std::move(data), {}};
        if (i == html.size()) return {};
    }
    if (html[i] != '<') {
        const auto end = html.find('<', i);
        const auto begin = i;
        i = end == std::string_view::npos ? html.size() : end;
        return {HtmlTokenKind::Text, decode_entities(html.substr(begin, i - begin)), {}};
    }
    if (html.compare(i, 4, "<!--") == 0) {
        const auto end = html.find("-->", i + 4);
        const auto begin = i + 4;
        const auto stop = end == std::string_view::npos ? html.size() : end;
        i = end == std::string_view::npos ? html.size() : end + 3;
        return {HtmlTokenKind::Comment, std::string(html.substr(begin, stop - begin)), {}};
    }
    if (html.compare(i, 2, "<!") == 0 || html.compare(i, 2, "<?") == 0) {
        const bool doctype = to_lower(html.substr(i + 2, 7)) == "doctype";
        const auto begin = i + (doctype ? 2 : 1);
        const auto end = html.find('>', begin);
        const auto stop = end == std::string_view::npos ? html.size() : end;
        i = end == std::string_view::npos ? html.size() : end + 1;
        return {doctype ? HtmlTokenKind::Doctype : HtmlTokenKind::Comment,
                std::string(html.substr(begin, stop - begin)), {}};
    }
    const bool closing = html.compare(i, 2, "</") == 0;
    auto j = i + (closing ? 2u : 1u);
    if (j >= html.size() || !ascii_alpha(html[j])) {
        ++i;
        return {HtmlTokenKind::Text, "<", {}};
    }
    const auto begin = j;
    while (j < html.size() && !is_space(html[j]) && html[j] != '/' && html[j] != '>') ++j;
    HtmlToken token{closing ? HtmlTokenKind::EndTag : HtmlTokenKind::StartTag,
                    to_lower(html.substr(begin, j - begin)), {}};
    std::unordered_set<std::string> seen;
    while (j < html.size() && html[j] != '>') {
        if (is_space(html[j]) || html[j] == '/') { ++j; continue; }
        const auto attr_begin = j;
        while (j < html.size() && !is_space(html[j]) && html[j] != '/' &&
               html[j] != '>' && html[j] != '=') ++j;
        if (j == attr_begin) { ++j; continue; }
        auto name = to_lower(html.substr(attr_begin, j - attr_begin));
        while (j < html.size() && is_space(html[j])) ++j;
        std::string value;
        if (j < html.size() && html[j] == '=') {
            ++j;
            while (j < html.size() && is_space(html[j])) ++j;
            if (j < html.size() && (html[j] == '\'' || html[j] == '"')) {
                const auto quote = html[j++];
                const auto value_begin = j;
                const auto end = html.find(quote, j);
                j = end == std::string_view::npos ? html.size() : end;
                value = decode_entities(html.substr(value_begin, j - value_begin));
                if (j < html.size()) ++j;
            } else {
                const auto value_begin = j;
                while (j < html.size() && !is_space(html[j]) && html[j] != '>') ++j;
                value = decode_entities(html.substr(value_begin, j - value_begin));
            }
        }
        if (!closing && seen.insert(name).second)
            token.attributes.push_back({std::move(name), std::move(value)});
    }
    i = j < html.size() ? j + 1 : j;
    return token;
}
}
