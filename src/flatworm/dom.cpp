#include "prowsetk/document.hpp"

#include <algorithm>

#include "flatworm/css_selector.hpp"
#include "flatworm/dom_internal.hpp"
#include "prowsetk/url.hpp"

namespace prowsetk {
namespace {

namespace fw = flatworm;

std::shared_ptr<Element> wrap(const std::shared_ptr<fw::Node>& node) {
    if (node == nullptr) {
        return nullptr;
    }
    return std::make_shared<Element>(node);
}

bool is_element_node(const std::shared_ptr<fw::Node>& node) {
    return node != nullptr && node->is_element();
}

}  // namespace

Element::Element(std::shared_ptr<flatworm::Node> node) : node_(std::move(node)) {}

std::string Element::tag_name() const {
    return node_ != nullptr ? node_->name : std::string();
}

std::string Element::id() const {
    return attribute("id");
}

std::string Element::class_name() const {
    return attribute("class");
}

bool Element::has_attribute(std::string_view name) const {
    return node_ != nullptr && node_->attribute(name) != nullptr;
}

std::string Element::attribute(std::string_view name) const {
    if (node_ == nullptr) {
        return {};
    }
    const std::string* value = node_->attribute(name);
    return value != nullptr ? *value : std::string();
}

void Element::set_attribute(std::string_view name, std::string_view value) {
    if (node_ != nullptr) {
        node_->set_attribute(name, value);
    }
}

bool Element::remove_attribute(std::string_view name) {
    return node_ != nullptr && node_->remove_attribute(name);
}

std::vector<Attribute> Element::attributes() const {
    std::vector<Attribute> result;
    if (node_ == nullptr) {
        return result;
    }
    result.reserve(node_->attributes.size());
    for (const auto& attr : node_->attributes) {
        result.push_back(Attribute{attr.name, attr.value});
    }
    return result;
}

std::string Element::text() const {
    return node_ != nullptr ? fw::text_content(*node_) : std::string();
}

std::string Element::inner_html() const {
    if (node_ == nullptr) {
        return {};
    }
    std::string result;
    for (const auto& child : node_->children) {
        result += fw::serialize(*child);
    }
    return result;
}

std::string Element::outer_html() const {
    return node_ != nullptr ? fw::serialize(*node_) : std::string();
}

std::shared_ptr<Element> Element::parent() const {
    if (node_ == nullptr) {
        return nullptr;
    }
    auto parent = node_->shared_parent();
    if (!is_element_node(parent)) {
        return nullptr;
    }
    return wrap(parent);
}

std::vector<std::shared_ptr<Element>> Element::children() const {
    std::vector<std::shared_ptr<Element>> result;
    if (node_ == nullptr) {
        return result;
    }
    for (const auto& child : node_->children) {
        if (child->is_element()) {
            result.push_back(wrap(child));
        }
    }
    return result;
}

std::shared_ptr<Element> Element::first_child() const {
    if (node_ == nullptr) {
        return nullptr;
    }
    for (const auto& child : node_->children) {
        if (child->is_element()) {
            return wrap(child);
        }
    }
    return nullptr;
}

std::shared_ptr<Element> Element::next_sibling() const {
    if (node_ == nullptr) {
        return nullptr;
    }
    auto parent = node_->shared_parent();
    if (parent == nullptr) {
        return nullptr;
    }
    bool found = false;
    for (const auto& sibling : parent->children) {
        if (found && sibling->is_element()) {
            return wrap(sibling);
        }
        if (sibling.get() == node_.get()) {
            found = true;
        }
    }
    return nullptr;
}

std::shared_ptr<Element> Element::previous_sibling() const {
    if (node_ == nullptr) {
        return nullptr;
    }
    auto parent = node_->shared_parent();
    if (parent == nullptr) {
        return nullptr;
    }
    std::shared_ptr<fw::Node> previous;
    for (const auto& sibling : parent->children) {
        if (sibling.get() == node_.get()) {
            break;
        }
        if (sibling->is_element()) {
            previous = sibling;
        }
    }
    return wrap(previous);
}

std::shared_ptr<Element> Element::query_selector(
    std::string_view selector) const {
    if (node_ == nullptr) {
        return nullptr;
    }
    auto result = fw::query_selector(node_, selector);
    return wrap(result);
}

std::vector<std::shared_ptr<Element>> Element::query_selector_all(
    std::string_view selector) const {
    std::vector<std::shared_ptr<Element>> result;
    if (node_ == nullptr) {
        return result;
    }
    for (const auto& node : fw::query_selector_all(node_, selector)) {
        result.push_back(wrap(node));
    }
    return result;
}

bool Element::matches(std::string_view selector) const {
    if (node_ == nullptr) {
        return false;
    }
    return fw::Selector::parse(selector).matches(*node_);
}

std::string Element::value() const {
    if (node_ == nullptr) {
        return {};
    }
    if (node_->name == "textarea") {
        return fw::text_content(*node_);
    }
    if (node_->name == "select") {
        for (const auto& option : fw::query_selector_all(node_, "option")) {
            if (option->attribute("selected") != nullptr) {
                const std::string* value = option->attribute("value");
                return value != nullptr ? *value : fw::text_content(*option);
            }
        }
        auto option = fw::query_selector(node_, "option");
        if (option != nullptr) {
            const std::string* value = option->attribute("value");
            return value != nullptr ? *value : fw::text_content(*option);
        }
        return {};
    }
    const std::string* value = node_->attribute("value");
    return value != nullptr ? *value : std::string();
}

void Element::set_value(std::string_view value) {
    if (node_ == nullptr) {
        return;
    }
    if (node_->name == "textarea") {
        set_text(value);
        return;
    }
    node_->set_attribute("value", value);
}

void Element::append_child(const std::shared_ptr<Element>& child) {
    if (node_ == nullptr || child == nullptr || child->node_ == nullptr) {
        return;
    }
    node_->append_child(child->node_);
}

bool Element::remove_child(const std::shared_ptr<Element>& child) {
    if (node_ == nullptr || child == nullptr || child->node_ == nullptr) {
        return false;
    }
    return node_->remove_child(child->node_);
}

void Element::set_text(std::string_view value) {
    if (node_ == nullptr) {
        return;
    }
    node_->children.clear();
    auto text = fw::make_node(fw::NodeType::Text);
    text->text = std::string(value);
    text->parent = node_;
    node_->children.push_back(text);
    node_->report_mutation(
        fw::MutationInfo{"text-set", node_->name, {}, std::string(value)});
}

Document::Document(std::shared_ptr<flatworm::Node> root)
    : root_(std::move(root)) {}

std::string Document::url() const { return url_; }
void Document::set_url(std::string url) { url_ = std::move(url); }

std::string Document::base_url() const {
    if (!base_url_.empty()) {
        return base_url_;
    }
    auto base = query_selector("base[href]");
    if (base != nullptr) {
        const std::string href = base->attribute("href");
        if (!href.empty()) {
            try {
                return url_.empty() ? href : resolve_url(url_, href);
            } catch (...) {
                return href;
            }
        }
    }
    return url_;
}

void Document::set_base_url(std::string base_url) {
    base_url_ = std::move(base_url);
}

std::string Document::title() const {
    auto title = query_selector("title");
    if (title != nullptr) {
        return title->text();
    }
    auto meta = query_selector("meta[property='og:title']");
    if (meta != nullptr) {
        return meta->attribute("content");
    }
    return {};
}

std::string Document::text() const {
    return root_ != nullptr ? fw::text_content(*root_) : std::string();
}

std::string Document::html() const {
    return root_ != nullptr ? fw::serialize(*root_) : std::string();
}

std::shared_ptr<Element> Document::root() const { return wrap(root_); }

std::shared_ptr<Element> Document::query_selector(
    std::string_view selector) const {
    if (root_ == nullptr) {
        return nullptr;
    }
    return wrap(fw::query_selector(root_, selector));
}

std::vector<std::shared_ptr<Element>> Document::query_selector_all(
    std::string_view selector) const {
    std::vector<std::shared_ptr<Element>> result;
    if (root_ == nullptr) {
        return result;
    }
    for (const auto& node : fw::query_selector_all(root_, selector)) {
        result.push_back(wrap(node));
    }
    return result;
}

std::shared_ptr<Element> Document::get_element_by_id(std::string_view id) const {
    return query_selector("#" + std::string(id));
}

std::vector<std::shared_ptr<Element>> Document::get_elements_by_tag_name(
    std::string_view tag) const {
    return query_selector_all(std::string(tag));
}

std::vector<std::shared_ptr<Element>> Document::links() const {
    std::vector<std::shared_ptr<Element>> result;
    for (const auto& element : query_selector_all("a[href], area[href]")) {
        result.push_back(element);
    }
    return result;
}

std::vector<std::shared_ptr<Element>> Document::forms() const {
    return query_selector_all("form");
}

std::vector<std::shared_ptr<Element>> Document::scripts() const {
    return query_selector_all("script");
}

std::vector<std::string> Document::resource_urls() const {
    std::vector<std::string> urls;
    for (const auto& element :
         query_selector_all("script[src], link[href], img[src], source[src], "
                            "iframe[src], video[src], audio[src]")) {
        std::string reference = element->attribute("src");
        if (reference.empty()) {
            reference = element->attribute("href");
        }
        if (reference.empty()) {
            continue;
        }
        try {
            const std::string base = base_url();
            urls.push_back(base.empty() ? reference
                                        : resolve_url(base, reference));
        } catch (...) {
            urls.push_back(reference);
        }
    }
    return urls;
}

std::map<std::string, std::string> Document::metadata() const {
    std::map<std::string, std::string> metadata;
    const std::string page_title = title();
    if (!page_title.empty()) {
        metadata["title"] = page_title;
    }
    for (const auto& meta : query_selector_all("meta")) {
        std::string key = meta->attribute("name");
        if (key.empty()) {
            key = meta->attribute("property");
        }
        if (key.empty()) {
            key = meta->attribute("http-equiv");
        }
        const std::string content = meta->attribute("content");
        if (!key.empty()) {
            metadata[key] = content;
        }
        const std::string charset = meta->attribute("charset");
        if (!charset.empty()) {
            metadata["charset"] = charset;
        }
    }
    auto html = query_selector("html");
    if (html != nullptr && html->has_attribute("lang")) {
        metadata["lang"] = html->attribute("lang");
    }
    return metadata;
}

std::shared_ptr<Element> Document::create_element(std::string tag) {
    auto node = fw::make_node(fw::NodeType::Element);
    node->name = std::move(tag);
    return wrap(node);
}

void Document::set_mutation_listener(
    std::function<void(const flatworm::MutationInfo&)> listener) {
    if (root_ == nullptr) {
        return;
    }
    if (listener) {
        root_->mutation_sink =
            std::make_shared<fw::MutationSink>(std::move(listener));
    } else {
        root_->mutation_sink.reset();
    }
}

std::shared_ptr<Document> parse_html(std::string_view html, std::string url,
                                     std::string base_url) {
    auto root = flatworm::parse_html_tree(html);
    auto document = std::make_shared<Document>(root);
    document->set_url(std::move(url));
    document->set_base_url(std::move(base_url));
    return document;
}

std::string serialize_node(const flatworm::Node& node) {
    return flatworm::serialize(node);
}

}  // namespace prowsetk
