#ifndef PROWSETK_FLATWORM_HOST_HPP
#define PROWSETK_FLATWORM_HOST_HPP

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "prowsetk/document.hpp"
#include "prowsetk/javascript_runtime.hpp"

namespace prowsetk {

namespace flatworm {
struct Node;
}

class KeyValueStore;

// The Flatworm-side implementation of `DocumentScriptHost`: it owns the
// node-handle registry that page JavaScript binds to and resolves every
// mediated operation (DOM queries and mutations, XHR/fetch requests, cookies,
// web storage, and script-initiated navigation) against the live document.
//
// A `Session` embeds one host per browsing context and wires the request,
// cookie, storage, and navigator hooks to the browser; embedders and tests can
// create a detached host for a standalone document (see
// `make_detached_script_host`).
class FlatwormScriptHost final : public DocumentScriptHost {
public:
    using RequestHook = std::function<HostResponse(const HostRequest&)>;
    using CookieHook = std::function<std::string()>;
    using CookieSetterHook = std::function<void(std::string_view)>;
    using StorageHook = std::function<KeyValueStore*(std::string_view area)>;
    using NavigatorHook = std::function<NavigatorInfo()>;

    FlatwormScriptHost();
    ~FlatwormScriptHost() override;

    // Replaces the live document (navigation or load_html). All outstanding
    // element handles become invalid and the registry is rebuilt lazily.
    void install(std::shared_ptr<Document> document, std::string base_url);

    void set_request_hook(RequestHook hook) { request_hook_ = std::move(hook); }
    void set_cookie_hooks(CookieHook get, CookieSetterHook set) {
        cookie_hook_ = std::move(get);
        cookie_setter_hook_ = std::move(set);
    }
    void set_storage_hook(StorageHook hook) { storage_hook_ = std::move(hook); }
    void set_navigator_hook(NavigatorHook hook) { navigator_hook_ = std::move(hook); }
    void set_referrer(std::string referrer) { referrer_ = std::move(referrer); }

    const std::shared_ptr<Document>& document() const noexcept {
        return document_;
    }
    // URL relative references resolve against: the configured base or the
    // document URL.
    std::string base_url() const override;

    // ---- DocumentScriptHost ----
    std::string document_element_class_name() const override;
    void set_document_element_class_name(std::string_view value) override;
    std::string document_url() const override;
    PageInfo page_info() const override;
    void set_document_title(std::string_view title) override;
    NavigatorInfo navigator_info() const override;
    HostResponse host_request(const HostRequest& request) override;
    void request_navigation(std::string_view url) override;
    void request_form_submission(std::string_view url, std::string_view method,
                                 std::string_view body) override;
    bool consume_pending_navigation(PendingNavigation& out) override;
    std::string cookie_header() const override;
    void set_cookie_string(std::string_view cookie) override;
    std::string storage_item(std::string_view area,
                             std::string_view key) const override;
    void set_storage_item(std::string_view area, std::string_view key,
                          std::string_view value) override;
    void remove_storage_item(std::string_view area,
                             std::string_view key) override;
    std::vector<std::string> storage_keys(std::string_view area) const override;
    void clear_storage(std::string_view area) override;

    std::vector<ElementHandle> query_selector_all(
        std::string_view selector) const override;
    std::vector<ElementHandle> query_selector_all_in(
        ElementHandle scope, std::string_view selector) const override;
    bool element_matches(ElementHandle element,
                         std::string_view selector) const override;
    ElementHandle root_element() const override;
    std::string element_tag_name(ElementHandle element) const override;
    int element_node_type(ElementHandle element) const override;
    std::string element_attribute(ElementHandle element,
                                  std::string_view name) const override;
    bool element_has_attribute(ElementHandle element,
                               std::string_view name) const override;
    std::vector<std::pair<std::string, std::string>>
    element_attributes(ElementHandle element) const override;
    void element_set_attribute(ElementHandle element, std::string_view name,
                               std::string_view value) override;
    void element_remove_attribute(ElementHandle element,
                                  std::string_view name) override;
    std::string element_text(ElementHandle element) const override;
    void element_set_text(ElementHandle element,
                          std::string_view text) override;
    std::string element_inner_html(ElementHandle element) const override;
    void element_set_inner_html(ElementHandle element,
                                std::string_view markup) override;
    std::string element_outer_html(ElementHandle element) const override;
    std::vector<ElementHandle> element_child_nodes(
        ElementHandle element) const override;
    ElementHandle element_parent(ElementHandle element) const override;
    ElementHandle element_next_sibling(ElementHandle element) const override;
    ElementHandle element_previous_sibling(
        ElementHandle element) const override;
    std::vector<ElementHandle> parse_html_fragment(
        std::string_view markup) const override;
    ElementHandle create_element(std::string_view tag_name) override;
    ElementHandle create_text_node(std::string_view text) override;
    ElementHandle create_comment(std::string_view text) override;
    bool append_child(ElementHandle parent, ElementHandle child) override;
    bool remove_child(ElementHandle parent, ElementHandle child) override;
    bool insert_before(ElementHandle parent, ElementHandle node,
                       ElementHandle reference) override;
    bool detach_element(ElementHandle node) override;

private:
    std::shared_ptr<flatworm::Node> resolve(ElementHandle handle) const;
    // Registers `node` (or returns its existing handle), keeping node identity
    // stable across evaluations.
    ElementHandle intern(const std::shared_ptr<flatworm::Node>& node) const;
    std::shared_ptr<flatworm::Node> document_element_node() const;
    std::shared_ptr<flatworm::Node> head_node() const;
    KeyValueStore* storage_area(std::string_view area) const;

    std::shared_ptr<Document> document_;
    std::string base_url_;
    std::string referrer_;
    PendingNavigation pending_;

    RequestHook request_hook_;
    CookieHook cookie_hook_;
    CookieSetterHook cookie_setter_hook_;
    StorageHook storage_hook_;
    NavigatorHook navigator_hook_;

    // Mutable because handles are interned lazily from const query paths.
    mutable std::unordered_map<ElementHandle, std::shared_ptr<flatworm::Node>>
        registry_;
    mutable std::map<const flatworm::Node*, ElementHandle> node_to_handle_;
    mutable ElementHandle next_handle_ = 1;
    // Fallback web storage when no backend hook is wired (detached hosts).
    std::map<std::string, std::string> fallback_local_;
    std::map<std::string, std::string> fallback_session_;
};

// A detached host over a standalone document: no cookies or browser storage
// beyond its own in-memory state, and requests go through `request` when
// provided. Used by embedders and by the unit tests of the JavaScript
// platform bindings.
std::unique_ptr<FlatwormScriptHost> make_detached_script_host(
    std::shared_ptr<Document> document,
    std::function<HostResponse(const HostRequest&)> request = {});

}  // namespace prowsetk

#endif  // PROWSETK_FLATWORM_HOST_HPP
