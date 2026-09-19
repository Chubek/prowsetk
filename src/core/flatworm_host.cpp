#include "prowsetk/flatworm_host.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

#include "flatworm/css_selector.hpp"
#include "flatworm/dom_internal.hpp"
#include "prowsetk/error.hpp"
#include "prowsetk/storage.hpp"
#include "prowsetk/url.hpp"

namespace prowsetk {
namespace {

namespace fw = flatworm;

std::string lower_case(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    return result;
}

int node_type_code(fw::NodeType type) {
    switch (type) {
        case fw::NodeType::Element: return 1;
        case fw::NodeType::Text: return 3;
        case fw::NodeType::Comment: return 8;
        case fw::NodeType::Document: return 9;
        case fw::NodeType::Doctype: return 10;
    }
    return 0;
}

}  // namespace

FlatwormScriptHost::FlatwormScriptHost() = default;
FlatwormScriptHost::~FlatwormScriptHost() = default;

void FlatwormScriptHost::install(std::shared_ptr<Document> document,
                                 std::string base_url) {
    document_ = std::move(document);
    base_url_ = std::move(base_url);
    if (base_url_.empty() && document_ != nullptr) {
        base_url_ = document_->base_url();
    }
    registry_.clear();
    node_to_handle_.clear();
    pending_ = PendingNavigation{};
}

std::shared_ptr<fw::Node> FlatwormScriptHost::resolve(
    ElementHandle handle) const {
    const auto found = registry_.find(handle);
    return found == registry_.end() ? nullptr : found->second;
}

ElementHandle FlatwormScriptHost::intern(
    const std::shared_ptr<fw::Node>& node) const {
    if (node == nullptr) return kNoElement;
    const auto existing = node_to_handle_.find(node.get());
    if (existing != node_to_handle_.end()) return existing->second;
    const ElementHandle handle = next_handle_++;
    registry_.emplace(handle, node);
    node_to_handle_.emplace(node.get(), handle);
    return handle;
}

std::shared_ptr<fw::Node> FlatwormScriptHost::document_element_node() const {
    if (document_ == nullptr) return nullptr;
    auto html = document_->query_selector("html");
    if (html != nullptr) return html->node();
    return document_->root() != nullptr ? document_->root()->node() : nullptr;
}

std::shared_ptr<fw::Node> FlatwormScriptHost::head_node() const {
    if (document_ == nullptr) return nullptr;
    auto head = document_->query_selector("head");
    return head != nullptr ? head->node() : nullptr;
}

std::string FlatwormScriptHost::base_url() const {
    if (!base_url_.empty()) return base_url_;
    return document_url();
}

std::string FlatwormScriptHost::document_element_class_name() const {
    auto root = document_element_node();
    if (root == nullptr) return {};
    const std::string* value = root->attribute("class");
    return value != nullptr ? *value : std::string{};
}

void FlatwormScriptHost::set_document_element_class_name(
    std::string_view value) {
    auto root = document_element_node();
    if (root != nullptr) root->set_attribute("class", value);
}

std::string FlatwormScriptHost::document_url() const {
    return document_ == nullptr ? std::string{} : document_->url();
}

PageInfo FlatwormScriptHost::page_info() const {
    PageInfo info;
    info.url = document_url();
    info.referrer = referrer_;
    info.title = document_ == nullptr ? std::string{} : document_->title();
    return info;
}

void FlatwormScriptHost::set_document_title(std::string_view title) {
    if (document_ == nullptr) return;
    if (auto existing = document_->query_selector("title");
        existing != nullptr) {
        existing->set_text(title);
        return;
    }
    auto head = head_node();
    if (head == nullptr) head = document_element_node();
    if (head == nullptr) return;
    auto node = fw::make_node(fw::NodeType::Element);
    node->name = "title";
    auto text = fw::make_node(fw::NodeType::Text);
    text->text = std::string{title};
    node->append_child(text);
    head->append_child(node);
}

NavigatorInfo FlatwormScriptHost::navigator_info() const {
    if (navigator_hook_) return navigator_hook_();
    return DocumentScriptHost::navigator_info();
}

HostResponse FlatwormScriptHost::host_request(const HostRequest& request) {
    if (request_hook_) return request_hook_(request);
    return DocumentScriptHost::host_request(request);
}

void FlatwormScriptHost::request_navigation(std::string_view url) {
    const std::string current = document_url().empty() ? base_url()
                                                       : document_url();
    if (url.empty()) return;
    if (url.front() == '#') {
        // Pure fragment change: the shim updates location in place; the
        // document stays loaded.
        return;
    }
    pending_ = PendingNavigation{};
    try {
        pending_.url = resolve_url(current, url);
    } catch (const Error&) {
        pending_.url = std::string{url};
    }
}

void FlatwormScriptHost::request_form_submission(std::string_view url,
                                                 std::string_view method,
                                                 std::string_view body) {
    const std::string current = document_url().empty() ? base_url()
                                                       : document_url();
    pending_ = PendingNavigation{};
    pending_.method = method.empty() ? "GET" : std::string{method};
    pending_.body = std::string{body};
    try {
        pending_.url = resolve_url(current, url);
    } catch (const Error&) {
        pending_.url = std::string{url};
    }
}

bool FlatwormScriptHost::consume_pending_navigation(PendingNavigation& out) {
    if (pending_.url.empty()) return false;
    out = pending_;
    pending_ = PendingNavigation{};
    return true;
}

std::string FlatwormScriptHost::cookie_header() const {
    if (cookie_hook_) return cookie_hook_();
    return {};
}

void FlatwormScriptHost::set_cookie_string(std::string_view cookie) {
    if (cookie_setter_hook_) cookie_setter_hook_(cookie);
}

KeyValueStore* FlatwormScriptHost::storage_area(std::string_view area) const {
    if (storage_hook_) return storage_hook_(area);
    return nullptr;
}

std::string FlatwormScriptHost::storage_item(std::string_view area,
                                             std::string_view key) const {
    if (auto* store = storage_area(area); store != nullptr) {
        const auto found = store->get(key);
        return found.has_value() ? *found : std::string{};
    }
    const auto& map = area == "session" ? fallback_session_ : fallback_local_;
    const auto found = map.find(std::string{key});
    return found == map.end() ? std::string{} : found->second;
}

void FlatwormScriptHost::set_storage_item(std::string_view area,
                                          std::string_view key,
                                          std::string_view value) {
    if (auto* store = storage_area(area); store != nullptr) {
        store->set(std::string{key}, std::string{value});
        return;
    }
    auto& map = area == "session" ? fallback_session_ : fallback_local_;
    map[std::string{key}] = std::string{value};
}

void FlatwormScriptHost::remove_storage_item(std::string_view area,
                                             std::string_view key) {
    if (auto* store = storage_area(area); store != nullptr) {
        store->remove(key);
        return;
    }
    auto& map = area == "session" ? fallback_session_ : fallback_local_;
    map.erase(std::string{key});
}

std::vector<std::string> FlatwormScriptHost::storage_keys(
    std::string_view area) const {
    if (auto* store = storage_area(area); store != nullptr) {
        return store->keys();
    }
    std::vector<std::string> keys;
    const auto& map = area == "session" ? fallback_session_ : fallback_local_;
    for (const auto& [key, value] : map) {
        keys.push_back(key);
    }
    return keys;
}

void FlatwormScriptHost::clear_storage(std::string_view area) {
    if (auto* store = storage_area(area); store != nullptr) {
        store->clear();
        return;
    }
    if (area == "session") {
        fallback_session_.clear();
    } else {
        fallback_local_.clear();
    }
}

std::vector<ElementHandle> FlatwormScriptHost::query_selector_all(
    std::string_view selector) const {
    std::vector<ElementHandle> handles;
    if (document_ == nullptr) return handles;
    for (const auto& element : document_->query_selector_all(selector)) {
        handles.push_back(intern(element->node()));
    }
    return handles;
}

std::vector<ElementHandle> FlatwormScriptHost::query_selector_all_in(
    ElementHandle scope, std::string_view selector) const {
    std::vector<ElementHandle> handles;
    auto scope_node = resolve(scope);
    if (scope_node == nullptr) return handles;
    for (const auto& node : fw::query_selector_all(scope_node, selector)) {
        if (node == scope_node) continue;
        handles.push_back(intern(node));
    }
    return handles;
}

bool FlatwormScriptHost::element_matches(ElementHandle element,
                                         std::string_view selector) const {
    auto node = resolve(element);
    if (node == nullptr || !node->is_element()) return false;
    return fw::Selector::parse(selector).matches(*node);
}

ElementHandle FlatwormScriptHost::root_element() const {
    return intern(document_element_node());
}

std::string FlatwormScriptHost::element_tag_name(
    ElementHandle element) const {
    auto node = resolve(element);
    if (node == nullptr) return {};
    return node->is_element() ? node->name : std::string{"#text"};
}

int FlatwormScriptHost::element_node_type(ElementHandle element) const {
    auto node = resolve(element);
    return node == nullptr ? 0 : node_type_code(node->type);
}

std::string FlatwormScriptHost::element_attribute(ElementHandle element,
                                                  std::string_view name) const {
    auto node = resolve(element);
    if (node == nullptr) return {};
    const std::string* value = node->attribute(name);
    if (value == nullptr && lower_case(name) != name) {
        // The parser stores attributes verbatim; page scripts use lowercase.
        for (const auto& attr : node->attributes) {
            if (lower_case(attr.name) == lower_case(name)) return attr.value;
        }
    }
    return value != nullptr ? *value : std::string{};
}

bool FlatwormScriptHost::element_has_attribute(ElementHandle element,
                                               std::string_view name) const {
    auto node = resolve(element);
    if (node == nullptr) return false;
    if (node->attribute(name) != nullptr) return true;
    for (const auto& attr : node->attributes) {
        if (lower_case(attr.name) == lower_case(name)) return true;
    }
    return false;
}

std::vector<std::pair<std::string, std::string>>
FlatwormScriptHost::element_attributes(ElementHandle element) const {
    auto node = resolve(element);
    std::vector<std::pair<std::string, std::string>> result;
    if (node == nullptr) return result;
    for (const auto& attr : node->attributes) {
        result.emplace_back(attr.name, attr.value);
    }
    return result;
}

void FlatwormScriptHost::element_set_attribute(ElementHandle element,
                                               std::string_view name,
                                               std::string_view value) {
    auto node = resolve(element);
    if (node != nullptr) node->set_attribute(name, value);
}

void FlatwormScriptHost::element_remove_attribute(ElementHandle element,
                                                  std::string_view name) {
    auto node = resolve(element);
    if (node == nullptr) return;
    if (!node->remove_attribute(name)) {
        for (const auto& attr : node->attributes) {
            if (lower_case(attr.name) == lower_case(name)) {
                node->remove_attribute(attr.name);
                break;
            }
        }
    }
}

std::string FlatwormScriptHost::element_text(ElementHandle element) const {
    auto node = resolve(element);
    return node == nullptr ? std::string{} : fw::text_content(*node);
}

void FlatwormScriptHost::element_set_text(ElementHandle element,
                                          std::string_view text) {
    auto node = resolve(element);
    if (node == nullptr) return;
    for (const auto& child : node->children) {
        child->parent.reset();
    }
    node->children.clear();
    auto text_node = fw::make_node(fw::NodeType::Text);
    text_node->text = std::string{text};
    text_node->parent = node;
    node->children.push_back(text_node);
    node->report_mutation(
        fw::MutationInfo{"text-set", node->name, {}, std::string{text}});
}

std::string FlatwormScriptHost::element_inner_html(
    ElementHandle element) const {
    auto node = resolve(element);
    if (node == nullptr) return {};
    std::string result;
    for (const auto& child : node->children) {
        result += fw::serialize(*child);
    }
    return result;
}

void FlatwormScriptHost::element_set_inner_html(ElementHandle element,
                                                std::string_view markup) {
    auto node = resolve(element);
    if (node == nullptr) return;
    for (const auto& child : node->children) {
        child->parent.reset();
    }
    node->children.clear();
    auto fragment = fw::parse_html_tree(markup);
    if (fragment == nullptr) return;
    auto children = fragment->children;
    fragment->children.clear();
    for (const auto& child : children) {
        if (child->type == fw::NodeType::Doctype) continue;
        node->append_child(child);
    }
}

std::string FlatwormScriptHost::element_outer_html(
    ElementHandle element) const {
    auto node = resolve(element);
    return node == nullptr ? std::string{} : fw::serialize(*node);
}

std::vector<ElementHandle> FlatwormScriptHost::element_child_nodes(
    ElementHandle element) const {
    std::vector<ElementHandle> handles;
    auto node = resolve(element);
    if (node == nullptr) return handles;
    for (const auto& child : node->children) {
        handles.push_back(intern(child));
    }
    return handles;
}

ElementHandle FlatwormScriptHost::element_parent(ElementHandle element) const {
    auto node = resolve(element);
    if (node == nullptr) return kNoElement;
    return intern(node->shared_parent());
}

ElementHandle FlatwormScriptHost::element_next_sibling(
    ElementHandle element) const {
    auto node = resolve(element);
    auto parent = node == nullptr ? nullptr : node->shared_parent();
    if (parent == nullptr) return kNoElement;
    bool found = false;
    for (const auto& sibling : parent->children) {
        if (found) return intern(sibling);
        if (sibling.get() == node.get()) found = true;
    }
    return kNoElement;
}

ElementHandle FlatwormScriptHost::element_previous_sibling(
    ElementHandle element) const {
    auto node = resolve(element);
    auto parent = node == nullptr ? nullptr : node->shared_parent();
    if (parent == nullptr) return kNoElement;
    std::shared_ptr<fw::Node> previous;
    for (const auto& sibling : parent->children) {
        if (sibling.get() == node.get()) return intern(previous);
        previous = sibling;
    }
    return kNoElement;
}

std::vector<ElementHandle> FlatwormScriptHost::parse_html_fragment(
    std::string_view markup) const {
    std::vector<ElementHandle> handles;
    auto fragment = fw::parse_html_tree(markup);
    if (fragment == nullptr) return handles;
    for (const auto& child : fragment->children) {
        if (child->type == fw::NodeType::Doctype) continue;
        handles.push_back(intern(child));
    }
    return handles;
}

ElementHandle FlatwormScriptHost::create_element(std::string_view tag_name) {
    auto node = fw::make_node(fw::NodeType::Element);
    node->name = lower_case(tag_name);
    return intern(node);
}

ElementHandle FlatwormScriptHost::create_text_node(std::string_view text) {
    auto node = fw::make_node(fw::NodeType::Text);
    node->text = std::string{text};
    return intern(node);
}

ElementHandle FlatwormScriptHost::create_comment(std::string_view text) {
    auto node = fw::make_node(fw::NodeType::Comment);
    node->text = std::string{text};
    return intern(node);
}

bool FlatwormScriptHost::append_child(ElementHandle parent,
                                      ElementHandle child) {
    auto parent_node = resolve(parent);
    auto child_node = resolve(child);
    if (parent_node == nullptr || child_node == nullptr) return false;
    parent_node->append_child(child_node);
    return true;
}

bool FlatwormScriptHost::remove_child(ElementHandle parent,
                                      ElementHandle child) {
    auto parent_node = resolve(parent);
    auto child_node = resolve(child);
    if (parent_node == nullptr || child_node == nullptr) return false;
    return parent_node->remove_child(child_node);
}

bool FlatwormScriptHost::insert_before(ElementHandle parent, ElementHandle node,
                                       ElementHandle reference) {
    if (reference == kNoElement) return append_child(parent, node);
    auto parent_node = resolve(parent);
    auto node_ptr = resolve(node);
    auto reference_node = resolve(reference);
    if (parent_node == nullptr || node_ptr == nullptr ||
        reference_node == nullptr) {
        return false;
    }
    const auto position = std::find(parent_node->children.begin(),
                                    parent_node->children.end(), reference_node);
    if (position == parent_node->children.end()) {
        return append_child(parent, node);
    }
    const auto old_parent = node_ptr->shared_parent();
    if (old_parent != nullptr) old_parent->remove_child(node_ptr);
    node_ptr->parent = parent_node;
    parent_node->children.insert(position, node_ptr);
    parent_node->report_mutation(
        fw::MutationInfo{"child-added", node_ptr->name, {}, {}});
    return true;
}

bool FlatwormScriptHost::detach_element(ElementHandle node_handle) {
    auto node = resolve(node_handle);
    if (node == nullptr) return false;
    auto parent = node->shared_parent();
    if (parent == nullptr) return false;
    return parent->remove_child(node);
}

std::unique_ptr<FlatwormScriptHost> make_detached_script_host(
    std::shared_ptr<Document> document,
    std::function<HostResponse(const HostRequest&)> request) {
    auto host = std::make_unique<FlatwormScriptHost>();
    if (request) host->set_request_hook(std::move(request));
    host->install(std::move(document), {});
    return host;
}

}  // namespace prowsetk
