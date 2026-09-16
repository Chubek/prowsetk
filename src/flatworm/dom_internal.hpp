#ifndef PROWSETK_FLATWORM_DOM_INTERNAL_HPP
#define PROWSETK_FLATWORM_DOM_INTERNAL_HPP

#include <functional>
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

// A structured description of a DOM mutation, delivered to the owning
// document's mutation listener (README "Events and Hooks": DOM mutation).
struct MutationInfo {
    // One of: "child-added", "child-removed", "attribute-set",
    // "attribute-removed", "text-set".
    std::string kind;
    std::string node_name;
    std::string attribute_name;
    std::string value;
};

using MutationSink = std::function<void(const MutationInfo&)>;

// Internal DOM node. Children own their descendants through shared_ptr; parent
// links are weak to avoid reference cycles. Public Element handles share the
// same node pointers.
struct Node : std::enable_shared_from_this<Node> {
    NodeType type = NodeType::Element;
    std::string name;
    std::vector<Attribute> attributes;
    std::string text;
    std::vector<std::shared_ptr<Node>> children;
    std::weak_ptr<Node> parent;

    // Set only on the document root by the owning Document. Mutations anywhere
    // in the tree report through it.
    std::shared_ptr<MutationSink> mutation_sink;

    bool is_element() const noexcept { return type == NodeType::Element; }
    bool is_text() const noexcept { return type == NodeType::Text; }

    const std::string* attribute(std::string_view attribute_name) const;
    void set_attribute(std::string_view attribute_name, std::string_view value);
    bool remove_attribute(std::string_view attribute_name);

    std::shared_ptr<Node> shared_parent() const { return parent.lock(); }

    // Appends `child`, re-parents it, and reports a "child-added" mutation.
    void append_child(const std::shared_ptr<Node>& child);
    // Removes `child` from this node's children if present. Returns whether a
    // child was removed; reports a "child-removed" mutation.
    bool remove_child(const std::shared_ptr<Node>& child);

    // Walks up the weak parent chain to the owning document root.
    std::shared_ptr<Node> document_root() const;

    // Reports a mutation to the owning document's sink, if any.
    void report_mutation(const MutationInfo& info) const;
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