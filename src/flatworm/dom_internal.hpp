#ifndef PROWSETK_FLATWORM_DOM_INTERNAL_HPP
#define PROWSETK_FLATWORM_DOM_INTERNAL_HPP

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace prowsetk::flatworm {

enum class NodeType { Document, Element, Text, Comment, Doctype };

struct Attribute {
    std::string name;
    std::string value;
};

// Internal DOM node. Children own their descendants through shared_ptr; parent
// links are weak to avoid reference cycles. Public Element handles share the
// same node pointers.
struct Node {
    NodeType type = NodeType::Element;
    std::string name;
    std::vector<Attribute> attributes;
    std::string text;
    std::vector<std::shared_ptr<Node>> children;
    std::weak_ptr<Node> parent;

    bool is_element() const noexcept { return type == NodeType::Element; }
    bool is_text() const noexcept { return type == NodeType::Text; }

    const std::string* attribute(std::string_view attribute_name) const;
    void set_attribute(std::string_view attribute_name, std::string_view value);
    bool remove_attribute(std::string_view attribute_name);

    std::shared_ptr<Node> shared_parent() const { return parent.lock(); }
};

std::shared_ptr<Node> make_node(NodeType type);

// Serializes a node subtree back to HTML.
std::string serialize(const Node& node);

// Concatenates descendant text nodes, collapsing nothing.
std::string text_content(const Node& node);

// Parses a tolerant HTML document into a Document node.
std::shared_ptr<Node> parse_html_tree(std::string_view html);

}  // namespace prowsetk::flatworm

#endif  // PROWSETK_FLATWORM_DOM_INTERNAL_HPP
