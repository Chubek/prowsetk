#include "flatworm/dom_internal.hpp"
#include "prowsetk/error.hpp"
#ifdef PROWSETK_HAVE_LEXBOR
#include <lexbor/html/html.h>
#include <lexbor/dom/interfaces/character_data.h>
#endif
#ifdef PROWSETK_HAVE_GUMBO
#include <gumbo.h>
#endif

namespace prowsetk {
namespace {
using flatworm::Node;
using flatworm::NodeType;
void bound(std::size_t depth, std::size_t& count) {
    if (depth > 256 || ++count > 250000)
        throw Error(ErrorCode::ResourceLimit, "HTML tree exceeds node or depth limit");
}
#ifdef PROWSETK_HAVE_LEXBOR
std::shared_ptr<Node> copy_lexbor(lxb_dom_node_t* src, std::size_t depth, std::size_t& count) {
    bound(depth, count);
    auto dst = flatworm::make_node(NodeType::Document);
    if (src->type == LXB_DOM_NODE_TYPE_ELEMENT) {
        dst->type = NodeType::Element;
        auto* el = lxb_dom_interface_element(src);
        std::size_t n = 0;
        const auto* name = lxb_dom_element_local_name(el, &n);
        dst->name.assign(reinterpret_cast<const char*>(name), n);
        for (auto* a = lxb_dom_element_first_attribute(el); a; a = a->next) {
            name = lxb_dom_attr_local_name(a, &n);
            std::string key(reinterpret_cast<const char*>(name), n);
            const auto* value = lxb_dom_attr_value(a, &n);
            dst->attributes.push_back({key, value ? std::string(reinterpret_cast<const char*>(value), n) : ""});
        }
    } else if (src->type == LXB_DOM_NODE_TYPE_TEXT || src->type == LXB_DOM_NODE_TYPE_COMMENT) {
        dst->type = src->type == LXB_DOM_NODE_TYPE_TEXT ? NodeType::Text : NodeType::Comment;
        std::size_t len = 0;
        const auto* data = lxb_dom_node_text_content(src, &len);
        if (data) dst->text.assign(reinterpret_cast<const char*>(data), len);
    } else if (src->type == LXB_DOM_NODE_TYPE_DOCUMENT_TYPE) {
        dst->type = NodeType::Doctype;
        dst->text = "html";
    }
    for (auto* child = src->first_child; child; child = child->next) {
        auto converted = copy_lexbor(child, depth + 1, count);
        converted->parent = dst;
        dst->children.push_back(std::move(converted));
    }
    return dst;
}
#endif
#ifdef PROWSETK_HAVE_GUMBO
std::shared_ptr<Node> copy_gumbo(const GumboNode* src, std::size_t depth, std::size_t& count) {
    bound(depth, count);
    auto dst = flatworm::make_node(NodeType::Document);
    const GumboVector* children = nullptr;
    if (src->type == GUMBO_NODE_DOCUMENT) children = &src->v.document.children;
    else if (src->type == GUMBO_NODE_ELEMENT || src->type == GUMBO_NODE_TEMPLATE) {
        dst->type = NodeType::Element;
        dst->name = gumbo_normalized_tagname(src->v.element.tag);
        if (dst->name.empty()) {
            auto piece = src->v.element.original_tag;
            gumbo_tag_from_original_text(&piece);
            dst->name.assign(piece.data, piece.length);
            for (auto& c : dst->name) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
        }
        for (unsigned i = 0; i < src->v.element.attributes.length; ++i) {
            auto* a = static_cast<GumboAttribute*>(src->v.element.attributes.data[i]);
            dst->attributes.push_back({a->name, a->value});
        }
        children = &src->v.element.children;
    } else {
        dst->type = src->type == GUMBO_NODE_COMMENT ? NodeType::Comment : NodeType::Text;
        dst->text = src->v.text.text;
    }
    if (children) for (unsigned i = 0; i < children->length; ++i) {
        auto child = copy_gumbo(static_cast<GumboNode*>(children->data[i]), depth + 1, count);
        child->parent = dst;
        dst->children.push_back(std::move(child));
    }
    return dst;
}
#endif
}
bool html_parser_available(HtmlParser parser) noexcept {
    if (parser == HtmlParser::Builtin || parser == HtmlParser::Auto) return true;
#ifdef PROWSETK_HAVE_LEXBOR
    if (parser == HtmlParser::Lexbor) return true;
#endif
#ifdef PROWSETK_HAVE_GUMBO
    if (parser == HtmlParser::Gumbo) return true;
#endif
    return false;
}
std::shared_ptr<Document> parse_html_with(HtmlParser parser, std::string_view html,
                                        std::string url, std::string base_url) {
    if (html.size() > 16 * 1024 * 1024) throw Error(ErrorCode::ResourceLimit, "HTML input exceeds 16 MiB");
    if (parser == HtmlParser::Auto) {
        parser = html_parser_available(HtmlParser::Lexbor) ? HtmlParser::Lexbor :
                 html_parser_available(HtmlParser::Gumbo) ? HtmlParser::Gumbo : HtmlParser::Builtin;
    }
    if (parser == HtmlParser::Builtin) return parse_html(html, std::move(url), std::move(base_url));
    std::shared_ptr<Node> root;
    [[maybe_unused]] std::size_t count = 0;
#ifdef PROWSETK_HAVE_LEXBOR
    if (parser == HtmlParser::Lexbor) {
        std::unique_ptr<lxb_html_document_t, decltype(&lxb_html_document_destroy)> doc(
            lxb_html_document_create(), lxb_html_document_destroy);
        if (!doc || lxb_html_document_parse(doc.get(), reinterpret_cast<const lxb_char_t*>(html.data()), html.size()) != LXB_STATUS_OK)
            throw Error(ErrorCode::ParseError, "Lexbor document parse failed");
        root = copy_lexbor(lxb_dom_interface_node(doc.get()), 0, count);
    }
#endif
#ifdef PROWSETK_HAVE_GUMBO
    if (parser == HtmlParser::Gumbo) {
        auto destroy = [](GumboOutput* p) { gumbo_destroy_output(&kGumboDefaultOptions, p); };
        std::unique_ptr<GumboOutput, decltype(destroy)> doc(gumbo_parse_with_options(&kGumboDefaultOptions, html.data(), html.size()), destroy);
        if (!doc) throw Error(ErrorCode::ParseError, "Gumbo document parse failed");
        root = copy_gumbo(doc->document, 0, count);
    }
#endif
    if (!root) throw Error(ErrorCode::Unsupported, "Requested HTML parser unavailable");
    auto doc = std::make_shared<Document>(std::move(root));
    doc->set_url(std::move(url)); doc->set_base_url(std::move(base_url));
    return doc;
}
}
