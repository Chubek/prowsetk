#ifndef PROWSETK_DOCUMENT_HPP
#define PROWSETK_DOCUMENT_HPP

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace prowsetk {

namespace flatworm {
struct Node;
}  // namespace flatworm

struct Attribute {
    std::string name;
    std::string value;
};

// A DOM node. Cheap to copy (holds a shared reference to the underlying tree)
// and safe to keep alive after the originating Document is gone.
class Element {
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
