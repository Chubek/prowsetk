// ir_iml.cpp — encoding stratum (ProwseIML S-expression format).
//
// Sole owner of IML text serialization and macro expansion. Renders the
// Flatworm DOM as nested `(element ...)`, `(text ...)` forms inside a single
// `(document ...)` form; the tree walk here is a pure serializer over DOM
// structure, while positions and event semantics stay in the layout and
// semantics strata.

#include <algorithm>
#include <sstream>
#include <string_view>

#include "prowsetk/document.hpp"
#include "prowsetk/ir.hpp"

#include "flatworm/dom_internal.hpp"
#include "ir_internal.hpp"

namespace prowsetk {
namespace {

std::string escape_iml(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (const char c : text) {
        switch (c) {
            case '\\':
                out += "\\\\";
                break;
            case '"':
                out += "\\\"";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                out.push_back(c);
                break;
        }
    }
    return out;
}

void emit_iml_node(const std::shared_ptr<flatworm::Node>& node,
                   std::size_t indent, std::ostringstream& stream) {
    if (node == nullptr) {
        return;
    }
    const std::string spaces(indent, ' ');
    if (node->type == flatworm::NodeType::Document) {
        stream << "(document\n";
        for (const auto& child : node->children) {
            emit_iml_node(child, indent + 2, stream);
        }
        stream << ")\n";
        return;
    }
    if (node->type == flatworm::NodeType::Element) {
        stream << spaces << "(element " << node->name;
        if (!node->attributes.empty()) {
            stream << " (@";
            for (const auto& attr : node->attributes) {
                stream << " (" << attr.name << " \"" << escape_iml(attr.value)
                       << "\")";
            }
            stream << ")";
        }
        const bool has_element_child = std::any_of(
            node->children.begin(), node->children.end(), [](const auto& child) {
                return child != nullptr &&
                       child->type == flatworm::NodeType::Element;
            });
        const bool has_text_child = std::any_of(
            node->children.begin(), node->children.end(), [](const auto& child) {
                return child != nullptr &&
                       child->type == flatworm::NodeType::Text &&
                       ir::is_renderable_text(child->text);
            });
        if (!has_element_child && !has_text_child) {
            stream << ")\n";
            return;
        }
        stream << "\n";
        for (const auto& child : node->children) {
            emit_iml_node(child, indent + 2, stream);
        }
        stream << spaces << ")\n";
        return;
    }
    if (node->type == flatworm::NodeType::Text &&
        ir::is_renderable_text(node->text)) {
        stream << spaces << "(text \"" << escape_iml(node->text) << "\")\n";
    }
}

std::string unescape_iml_string(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '\\' || i + 1 >= text.size()) {
            out.push_back(text[i]);
            continue;
        }
        const char next = text[++i];
        switch (next) {
            case 'n':
                out.push_back('\n');
                break;
            case 'r':
                out.push_back('\r');
                break;
            case 't':
                out.push_back('\t');
                break;
            default:
                out.push_back(next);
                break;
        }
    }
    return out;
}

std::string parse_quoted(std::string_view text, std::size_t& pos) {
    if (pos >= text.size() || text[pos] != '"') {
        return {};
    }
    ++pos;
    std::string raw;
    while (pos < text.size()) {
        const char c = text[pos++];
        if (c == '"') {
            return unescape_iml_string(raw);
        }
        if (c == '\\' && pos < text.size()) {
            raw.push_back(c);
            raw.push_back(text[pos++]);
            continue;
        }
        raw.push_back(c);
    }
    return {};
}

void skip_spaces(std::string_view text, std::size_t& pos) {
    while (pos < text.size() &&
           (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\n' ||
            text[pos] == '\r')) {
        ++pos;
    }
}

bool parse_macro(std::string_view text, std::size_t start, std::size_t& end,
                 std::string& name, std::vector<std::string>& args) {
    std::size_t pos = start;
    if (text.substr(pos, 7) != "(macro ") {
        return false;
    }
    pos += 7;
    const std::size_t name_start = pos;
    while (pos < text.size() && text[pos] != ')' && text[pos] != ' ' &&
           text[pos] != '\t' && text[pos] != '\n' && text[pos] != '\r') {
        ++pos;
    }
    if (pos == name_start) {
        return false;
    }
    name.assign(text.substr(name_start, pos - name_start));
    while (true) {
        skip_spaces(text, pos);
        if (pos >= text.size()) {
            return false;
        }
        if (text[pos] == ')') {
            end = pos + 1;
            return true;
        }
        if (text[pos] != '"') {
            return false;
        }
        args.push_back(parse_quoted(text, pos));
    }
}

}  // namespace

std::string emit_prowse_iml(const Document& document) {
    if (!document.valid()) {
        return {};
    }
    std::ostringstream stream;
    emit_iml_node(document.raw_root(), 0, stream);
    return stream.str();
}

std::string expand_prowse_iml(std::string_view iml,
                              const ProwseImlMacroExpander& expander) {
    std::string out;
    out.reserve(iml.size());
    std::size_t pos = 0;
    while (pos < iml.size()) {
        if (iml[pos] != '(' || iml.substr(pos, 7) != "(macro ") {
            out.push_back(iml[pos++]);
            continue;
        }
        std::size_t end = pos;
        std::string name;
        std::vector<std::string> args;
        if (!parse_macro(iml, pos, end, name, args) || !expander) {
            out.push_back(iml[pos++]);
            continue;
        }
        const std::string expanded = expander(name, args);
        if (expanded.empty()) {
            out.append(iml.substr(pos, end - pos));
        } else {
            out.append(expanded);
        }
        pos = end;
    }
    return out;
}

}  // namespace prowsetk
