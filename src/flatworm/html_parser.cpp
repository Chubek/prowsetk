#include "flatworm/dom_internal.hpp"
#include "flatworm/html_syntax.hpp"
#include "flatworm/html_tokenizer.hpp"
#include "prowsetk/error.hpp"
#include <algorithm>
#include <initializer_list>

namespace prowsetk::flatworm {
namespace {
bool one_of(std::string_view name, std::initializer_list<std::string_view> names) {
    return std::find(names.begin(), names.end(), name) != names.end();
}
// Scoped, deliberately restricted HTML recovery. It does not implement the
// adoption-agency algorithm, foster parenting, or implicit document wrappers.
void close_for_start(std::vector<std::shared_ptr<Node>>& stack, std::string_view name) {
    const auto close_scoped = [&](std::initializer_list<std::string_view> targets,
                                  std::initializer_list<std::string_view> barriers) {
        for (auto depth = stack.size(); depth > 1; --depth) {
            const auto& open = stack[depth - 1]->name;
            if (one_of(open, targets)) { stack.resize(depth - 1); return; }
            if (one_of(open, barriers)) return;
        }
    };
    if (one_of(name, {"address", "article", "aside", "blockquote", "div", "dl", "fieldset",
                      "footer", "form", "h1", "h2", "h3", "h4", "h5", "h6", "header",
                      "hgroup", "hr", "main", "menu", "nav", "ol", "p", "pre", "section",
                      "table", "ul"}))
        close_scoped({"p"}, {"html", "table", "td", "th", "button", "template"});
    if (name == "li") close_scoped({"li"}, {"ul", "ol", "menu", "html", "template"});
    if (name == "dt" || name == "dd") close_scoped({"dt", "dd"}, {"dl", "html", "template"});
    if (name == "option" || name == "optgroup")
        close_scoped({"option"}, {"select", "datalist", "optgroup", "html", "template"});
    if (name == "optgroup") close_scoped({"optgroup"}, {"select", "html", "template"});
    if (one_of(name, {"td", "th", "tr", "tbody", "thead", "tfoot"}))
        close_scoped({"td", "th"}, {"tr", "table", "html", "template"});
    if (one_of(name, {"tr", "tbody", "thead", "tfoot"}))
        close_scoped({"tr"}, {"table", "tbody", "thead", "tfoot", "html", "template"});
    if (one_of(name, {"tbody", "thead", "tfoot"}))
        close_scoped({"tbody", "thead", "tfoot"}, {"table", "html", "template"});
}
}
std::shared_ptr<Node> parse_html_tree(std::string_view html) {
    // Limits also bound recursive consumers and shared_ptr subtree destruction.
    constexpr std::size_t max_bytes = 16 * 1024 * 1024;
    constexpr std::size_t max_nodes = 250000;
    constexpr std::size_t max_depth = 256;
    if (html.size() > max_bytes)
        throw Error(ErrorCode::ResourceLimit, "HTML input exceeds 16 MiB");
    auto root = make_node(NodeType::Document);
    std::vector<std::shared_ptr<Node>> stack{root};
    HtmlTokenizer tokenizer(html);
    std::size_t count = 1;
    while (true) {
        auto token = tokenizer.next();
        if (token.kind == HtmlTokenKind::End) break;
        if (token.kind == HtmlTokenKind::EndTag) {
            for (auto depth = stack.size(); depth > 1; --depth) {
                if (stack[depth - 1]->name == token.data) {
                    stack.resize(depth - 1); break;
                }
            }
            continue;
        }
        const bool element = token.kind == HtmlTokenKind::StartTag;
        if (element) close_for_start(stack, token.data);
        if (++count > max_nodes || stack.size() > max_depth)
            throw Error(ErrorCode::ResourceLimit, "HTML tree exceeds node or depth limit");
        const auto type = element ? NodeType::Element :
            token.kind == HtmlTokenKind::Text ? NodeType::Text :
            token.kind == HtmlTokenKind::Comment ? NodeType::Comment : NodeType::Doctype;
        auto node = make_node(type);
        if (element) {
            node->name = std::move(token.data);
            node->attributes = std::move(token.attributes);
        } else node->text = std::move(token.data);
        node->parent = stack.back();
        stack.back()->children.push_back(node);
        if (element && !is_void_element(node->name)) {
            stack.push_back(node);
            if (is_raw_text_element(node->name) || is_rcdata_element(node->name))
                tokenizer.text_element(node->name);
        }
    }
    return root;
}
}
