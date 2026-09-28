#include "flatworm/dom_internal.hpp"
#include "flatworm/html_syntax.hpp"

namespace prowsetk::flatworm {
namespace {
std::string escape_text(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (char c : value) {
        switch (c) {
            case '&': result += "&amp;"; break;
            case '<': result += "&lt;"; break;
            case '>': result += "&gt;"; break;
            default: result.push_back(c); break;
        }
    }
    return result;
}

std::string escape_attribute(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (char c : value) {
        switch (c) {
            case '&': result += "&amp;"; break;
            case '"': result += "&quot;"; break;
            case '<': result += "&lt;"; break;
            case '>': result += "&gt;"; break;
            default: result.push_back(c); break;
        }
    }
    return result;
}

std::string serialize_impl(const Node& node, std::string_view parent_name) {
    switch (node.type) {
        case NodeType::Document: {
            std::string result;
            for (const auto& child : node.children) {
                result += serialize_impl(*child, {});
            }
            return result;
        }
        case NodeType::Text:
            return is_raw_text_element(parent_name) ? node.text
                                                    : escape_text(node.text);
        case NodeType::Comment:
            return "<!--" + node.text + "-->";
        case NodeType::Doctype:
            return "<!" + node.text + ">";
        case NodeType::Element:
            break;
    }

    std::string result = "<" + node.name;
    for (const auto& attr : node.attributes) {
        result += " " + attr.name + "=\"" + escape_attribute(attr.value) + "\"";
    }
    if (is_void_element(node.name)) {
        result += ">";
        return result;
    }
    result += ">";
    for (const auto& child : node.children) {
        result += serialize_impl(*child, node.name);
    }
    result += "</" + node.name + ">";
    return result;
}

}
std::string serialize(const Node& node) {
    const auto parent = node.shared_parent();
    return serialize_impl(node, parent != nullptr ? parent->name : std::string_view{});
}

std::string text_content(const Node& node) {
    if (node.type == NodeType::Text) {
        return node.text;
    }
    std::string result;
    for (const auto& child : node.children) {
        result += text_content(*child);
    }
    return result;
}


}  // namespace prowsetk::flatworm
