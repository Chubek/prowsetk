#include "prowsetk/javascript_runtime.hpp"

#include "prowsetk/error.hpp"

namespace prowsetk {
namespace {

class NullJavaScriptRuntime : public JavaScriptRuntime {
public:
    ScriptResult evaluate(std::string_view script,
                          const ScriptOptions& options) override {
        (void)script;
        (void)options;
        return ScriptResult{false, {},
                            "JavaScript runtime is not available in this build"};
    }

    void set_global(std::string_view name, std::string_view value) override {
        (void)name;
        (void)value;
    }

    void set_console_handler(ConsoleHandler handler) override {
        (void)handler;
    }

    std::string name() const override { return "null"; }

    CapabilitySet capabilities() const override {
        CapabilitySet capabilities;
        capabilities.set("javascript", ImplementationClass::Unsupported,
                         "no JavaScript engine linked into this build");
        capabilities.set("console", ImplementationClass::Unsupported);
        capabilities.set("fetch", ImplementationClass::Unsupported);
        return capabilities;
    }
};

}  // namespace

// ---- DocumentScriptHost defaults ------------------------------------------
// A host may implement only the three pure-virtual methods (the historical
// minimal bridge). Every web-platform capability below degrades to a safe,
// deterministic empty so partial hosts and embedders keep compiling and the
// null runtime stays usable.

std::string DocumentScriptHost::base_url() const { return document_url(); }

PageInfo DocumentScriptHost::page_info() const {
    return PageInfo{document_url(), {}, {}};
}

void DocumentScriptHost::set_document_title(std::string_view) {}

NavigatorInfo DocumentScriptHost::navigator_info() const {
    NavigatorInfo info;
    info.user_agent =
        "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/128.0.0.0 Safari/537.36";
    info.platform = "Linux x86_64";
    info.language = "en-US";
    return info;
}

HostResponse DocumentScriptHost::host_request(const HostRequest&) {
    HostResponse response;
    response.error = "host-mediated requests are unavailable";
    return response;
}

void DocumentScriptHost::request_navigation(std::string_view) {}

void DocumentScriptHost::request_form_submission(std::string_view,
                                                 std::string_view,
                                                 std::string_view) {}

bool DocumentScriptHost::consume_pending_navigation(PendingNavigation&) {
    return false;
}

std::string DocumentScriptHost::cookie_header() const { return {}; }

void DocumentScriptHost::set_cookie_string(std::string_view) {}

std::string DocumentScriptHost::storage_item(std::string_view,
                                             std::string_view) const {
    return {};
}

void DocumentScriptHost::set_storage_item(std::string_view, std::string_view,
                                          std::string_view) {}

void DocumentScriptHost::remove_storage_item(std::string_view,
                                             std::string_view) {}

std::vector<std::string> DocumentScriptHost::storage_keys(std::string_view) const {
    return {};
}

void DocumentScriptHost::clear_storage(std::string_view) {}

std::vector<ElementHandle> DocumentScriptHost::query_selector_all(
    std::string_view) const {
    return {};
}

std::vector<ElementHandle> DocumentScriptHost::query_selector_all_in(
    ElementHandle, std::string_view) const {
    return {};
}

bool DocumentScriptHost::element_matches(ElementHandle, std::string_view) const {
    return false;
}

ElementHandle DocumentScriptHost::root_element() const { return kNoElement; }

std::string DocumentScriptHost::element_tag_name(ElementHandle) const {
    return {};
}

int DocumentScriptHost::element_node_type(ElementHandle) const { return 0; }

std::string DocumentScriptHost::element_attribute(ElementHandle,
                                                  std::string_view) const {
    return {};
}

bool DocumentScriptHost::element_has_attribute(ElementHandle,
                                               std::string_view) const {
    return false;
}

std::vector<std::pair<std::string, std::string>>
DocumentScriptHost::element_attributes(ElementHandle) const {
    return {};
}

void DocumentScriptHost::element_set_attribute(ElementHandle, std::string_view,
                                               std::string_view) {}

void DocumentScriptHost::element_remove_attribute(ElementHandle,
                                                  std::string_view) {}

std::string DocumentScriptHost::element_text(ElementHandle) const { return {}; }

void DocumentScriptHost::element_set_text(ElementHandle, std::string_view) {}

std::string DocumentScriptHost::element_inner_html(ElementHandle) const {
    return {};
}

void DocumentScriptHost::element_set_inner_html(ElementHandle,
                                                std::string_view) {}

std::string DocumentScriptHost::element_outer_html(ElementHandle) const {
    return {};
}

std::vector<ElementHandle> DocumentScriptHost::element_child_nodes(
    ElementHandle) const {
    return {};
}

ElementHandle DocumentScriptHost::element_parent(ElementHandle) const {
    return kNoElement;
}

ElementHandle DocumentScriptHost::element_next_sibling(ElementHandle) const {
    return kNoElement;
}

ElementHandle DocumentScriptHost::element_previous_sibling(ElementHandle) const {
    return kNoElement;
}

std::vector<ElementHandle> DocumentScriptHost::parse_html_fragment(
    std::string_view) const {
    return {};
}

ElementHandle DocumentScriptHost::create_element(std::string_view) {
    return kNoElement;
}

ElementHandle DocumentScriptHost::create_text_node(std::string_view) {
    return kNoElement;
}

ElementHandle DocumentScriptHost::create_comment(std::string_view) {
    return kNoElement;
}

bool DocumentScriptHost::append_child(ElementHandle, ElementHandle) {
    return false;
}

bool DocumentScriptHost::remove_child(ElementHandle, ElementHandle) {
    return false;
}

bool DocumentScriptHost::insert_before(ElementHandle, ElementHandle,
                                       ElementHandle) {
    return false;
}

bool DocumentScriptHost::detach_element(ElementHandle) { return false; }

// ---- Runtime base ----------------------------------------------------------

ScriptResult JavaScriptRuntime::run_microtasks(const ScriptOptions&) {
    return {false, {}, "JavaScript microtasks are not available in this runtime"};
}

bool JavaScriptRuntime::has_pending_microtasks() const {
    return false;
}

void JavaScriptRuntime::set_document_host(DocumentScriptHost*) {}

std::unique_ptr<JavaScriptRuntime> make_null_javascript_runtime() {
    return std::make_unique<NullJavaScriptRuntime>();
}

}  // namespace prowsetk
