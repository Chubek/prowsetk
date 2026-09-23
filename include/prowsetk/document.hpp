#ifndef PROWSETK_DOCUMENT_HPP
#define PROWSETK_DOCUMENT_HPP

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace prowsetk {

namespace flatworm {

struct Node;

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

}  // namespace flatworm

struct Attribute {
    std::string name;
    std::string value;
};

// A DOM node. Cheap to copy (holds a shared reference to the underlying tree)
// and safe to keep alive after the originating Document is gone.
class Element : public std::enable_shared_from_this<Element> {
public:
    Element() = default;
    explicit Element(std::shared_ptr<flatworm::Node> node);

    bool valid() const noexcept { return node_ != nullptr; }

    std::string tag_name() const;
    std::string id() const;
    std::string class_name() const;

    bool has_attribute(std::string_view name) const;
    std::string attribute(std::string_view name) const;
    void set_attribute(std::string_view name, std::string_view value);
    bool remove_attribute(std::string_view name);
    std::vector<Attribute> attributes() const;

    std::string text() const;
    std::string inner_html() const;
    std::string outer_html() const;

    std::shared_ptr<Element> parent() const;
    std::vector<std::shared_ptr<Element>> children() const;
    std::shared_ptr<Element> first_child() const;
    std::shared_ptr<Element> next_sibling() const;
    std::shared_ptr<Element> previous_sibling() const;

    std::shared_ptr<Element> query_selector(std::string_view selector) const;
    std::vector<std::shared_ptr<Element>> query_selector_all(
        std::string_view selector) const;
    bool matches(std::string_view selector) const;

    // Form-control value: the `value` attribute for input elements, the text
    // content for textarea, and the selected option for select elements.
    std::string value() const;
    void set_value(std::string_view value);

    // Synthetic-interaction fail-fast gates (README "Synthetic Interaction
    // Driver & SPA Event Cascades", section 3). A standalone Element carries
    // no script host, so these never dispatch: they return false for an
    // invalid or non-interactable element (hidden, disabled, or hidden
    // ancestor) and false otherwise to signal that no script dispatch
    // occurred. The full event cascades with microtask draining run through
    // Session::click_element and Session::type_element.
    bool click();
    bool type(std::string_view text);
    bool click(class Session& session);
    bool type(class Session& session, std::string_view text);

    // DOM mutation. `append_child` re-parents `child` into this element;
    // `remove_child` detaches it; `set_text` replaces the element's children
    // with a single text node. Each operation reports a DomMutation through
    // the owning document's mutation listener.
    void append_child(const std::shared_ptr<Element>& child);
    bool remove_child(const std::shared_ptr<Element>& child);
    void set_text(std::string_view value);

    const std::shared_ptr<flatworm::Node>& node() const noexcept { return node_; }

private:
    std::shared_ptr<flatworm::Node> node_;
};

// The currently loaded document. Produced by the HTML parser and owned by a
// session for the lifetime of a navigation.
class Document {
public:
    Document() = default;
    explicit Document(std::shared_ptr<flatworm::Node> root);

    bool valid() const noexcept { return root_ != nullptr; }

    std::string url() const;
    void set_url(std::string url);
    std::string base_url() const;
    void set_base_url(std::string base_url);

    std::string title() const;
    std::string text() const;
    std::string html() const;

    std::shared_ptr<Element> root() const;

    std::shared_ptr<Element> query_selector(std::string_view selector) const;
    std::vector<std::shared_ptr<Element>> query_selector_all(
        std::string_view selector) const;
    std::shared_ptr<Element> get_element_by_id(std::string_view id) const;
    std::vector<std::shared_ptr<Element>> get_elements_by_tag_name(
        std::string_view tag) const;

    // Discovery helpers used by scraping and endpoint extraction.
    std::vector<std::shared_ptr<Element>> links() const;
    std::vector<std::shared_ptr<Element>> forms() const;
    std::vector<std::shared_ptr<Element>> scripts() const;
    std::vector<std::string> resource_urls() const;
    std::map<std::string, std::string> metadata() const;

    // Creates a detached element that is not yet part of the document tree.
    // Attach it with Element::append_child on a live element.
    std::shared_ptr<Element> create_element(std::string tag);

    // Registers a listener invoked for every DOM mutation in this document's
    // tree. A Session installs one to emit EventType::DomMutation events.
    void set_mutation_listener(
        std::function<void(const flatworm::MutationInfo&)> listener);

    const std::shared_ptr<flatworm::Node>& raw_root() const noexcept { return root_; }

private:
    std::shared_ptr<flatworm::Node> root_;
    std::string url_;
    std::string base_url_;
};

// Parses `html` into a Document. The parser is tolerant and never throws on
// malformed markup; it is not a full HTML5 tree-construction implementation.
std::shared_ptr<Document> parse_html(std::string_view html,
                                     std::string url = {},
                                     std::string base_url = {});

// Serializes a node subtree back to HTML.
std::string serialize_node(const flatworm::Node& node);

}  // namespace prowsetk

#endif  // PROWSETK_DOCUMENT_HPP
