#ifndef PROWSETK_JAVASCRIPT_RUNTIME_HPP
#define PROWSETK_JAVASCRIPT_RUNTIME_HPP

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "prowsetk/capability.hpp"

namespace prowsetk {

struct ScriptOptions {
    int timeout_ms = 5000;
    std::size_t memory_limit_bytes = std::size_t{16} * 1024u * 1024u;
    std::size_t max_microtask_jobs = 10000;
};

struct ScriptResult {
    bool ok = false;
    std::string value;
    std::string error;
};

// A console message forwarded from page JavaScript (README "Events and Hooks":
// console message). `level` is one of "log", "info", "warn", "error", "debug".
struct ConsoleMessage {
    std::string level;
    std::string text;
};

using ConsoleHandler = std::function<void(const ConsoleMessage&)>;

// Stable reference to a DOM node as seen from page JavaScript. Handles are
// minted by the host, invalidated when the document is replaced, and never
// expose engine internals (README "JavaScript Execution").
using ElementHandle = std::uint64_t;
constexpr ElementHandle kNoElement = 0;

// One outbound request made by page script (XMLHttpRequest, fetch, or a
// dynamically injected <script src>). Executed host-mediated: the owning
// Session applies cookies, redirects, events, plugins, and redaction exactly
// as it does for navigations.
struct HostRequest {
    std::string method = "GET";
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
};

struct HostResponse {
    bool ok = false;
    std::string error;
    int status = 0;
    std::string status_text;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    std::string final_url;
};

// Snapshot of page metadata exposed to script through `location`,
// `document.referrer`, and `document.title`.
struct PageInfo {
    std::string url;
    std::string referrer;
    std::string title;
};

struct NavigatorInfo {
    std::string user_agent;
    std::string platform;
    std::string language;
    bool cookie_enabled = true;
    bool on_line = true;
};

// Script-initiated navigation recorded by the host: a GET navigation (link
// click, location.assign) or a form POST with an encoded body. The engine
// performs it after the current script pass completes. Declared before
// `DocumentScriptHost` (see `request_navigation`).
struct PendingNavigation {
    std::string url;
    std::string method = "GET";
    std::string body;
};

// The document host exposed to page JavaScript. JavaScript remains page
// scripting only: every DOM, network, storage, and navigation operation is
// mediated through these callbacks instead of exposing engine internals or
// C++ objects across the QuickJS boundary.
//
// `FlatwormScriptHost` is the real implementation (see flatworm_host.hpp);
// the defaults here keep partial hosts (and the null runtime) working, and
// degrade deterministically.
class DocumentScriptHost {
public:
    virtual ~DocumentScriptHost() = default;

    virtual std::string document_element_class_name() const = 0;
    virtual void set_document_element_class_name(std::string_view value) = 0;
    virtual std::string document_url() const = 0;
    // URL relative script references resolve against (base elements included).
    virtual std::string base_url() const;

    // Page metadata: `location`, `document.referrer`, `document.title`.
    virtual PageInfo page_info() const;
    virtual void set_document_title(std::string_view title);
    virtual NavigatorInfo navigator_info() const;

    // Host-mediated outbound request used by XMLHttpRequest and fetch.
    virtual HostResponse host_request(const HostRequest& request);

    // Script-initiated navigation (location.assign/replace/href setter, link
    // clicks, and form submissions). The host records the target; the engine
    // performs it after the current script pass completes.
    virtual void request_navigation(std::string_view url);
    virtual void request_form_submission(std::string_view url,
                                         std::string_view method,
                                         std::string_view body);
    virtual bool consume_pending_navigation(PendingNavigation& out);

    // document.cookie: a jar snapshot for the current host and one Set-Cookie
    // style string applied to it.
    virtual std::string cookie_header() const;
    virtual void set_cookie_string(std::string_view cookie);

    // localStorage / sessionStorage of the current browsing context. `area`
    // is "local" or "session".
    virtual std::string storage_item(std::string_view area,
                                     std::string_view key) const;
    virtual void set_storage_item(std::string_view area, std::string_view key,
                                  std::string_view value);
    virtual void remove_storage_item(std::string_view area,
                                     std::string_view key);
    virtual std::vector<std::string> storage_keys(std::string_view area) const;
    virtual void clear_storage(std::string_view area);

    // ---- Handle-based DOM bridge -------------------------------------
    // Selectors are CSS, resolved by the Flatworm selector engine. Handles
    // for nodes that were never registered resolve to kNoElement, empty
    // strings, or false.
    virtual std::vector<ElementHandle> query_selector_all(
        std::string_view selector) const;
    virtual std::vector<ElementHandle> query_selector_all_in(
        ElementHandle scope, std::string_view selector) const;
    virtual bool element_matches(ElementHandle element,
                                 std::string_view selector) const;
    virtual ElementHandle root_element() const;
    virtual std::string element_tag_name(ElementHandle element) const;
    virtual int element_node_type(ElementHandle element) const;
    virtual std::string element_attribute(ElementHandle element,
                                          std::string_view name) const;
    virtual bool element_has_attribute(ElementHandle element,
                                       std::string_view name) const;
    virtual std::vector<std::pair<std::string, std::string>>
    element_attributes(ElementHandle element) const;
    virtual void element_set_attribute(ElementHandle element,
                                       std::string_view name,
                                       std::string_view value);
    virtual void element_remove_attribute(ElementHandle element,
                                          std::string_view name);
    virtual std::string element_text(ElementHandle element) const;
    virtual void element_set_text(ElementHandle element,
                                  std::string_view text);
    virtual std::string element_inner_html(ElementHandle element) const;
    virtual void element_set_inner_html(ElementHandle element,
                                        std::string_view markup);
    virtual std::string element_outer_html(ElementHandle element) const;
    virtual std::vector<ElementHandle> element_child_nodes(
        ElementHandle element) const;
    virtual ElementHandle element_parent(ElementHandle element) const;
    virtual ElementHandle element_next_sibling(ElementHandle element) const;
    virtual ElementHandle element_previous_sibling(
        ElementHandle element) const;
    // Parses `markup` and returns the resulting nodes as detached handles
    // (used by DOMParser and <template>).
    virtual std::vector<ElementHandle> parse_html_fragment(
        std::string_view markup) const;
    virtual ElementHandle create_element(std::string_view tag_name);
    virtual ElementHandle create_text_node(std::string_view text);
    virtual ElementHandle create_comment(std::string_view text);
    virtual bool append_child(ElementHandle parent, ElementHandle child);
    virtual bool remove_child(ElementHandle parent, ElementHandle child);
    virtual bool insert_before(ElementHandle parent, ElementHandle node,
                               ElementHandle reference);
    virtual bool detach_element(ElementHandle node);
};

// Executes page JavaScript. JavaScript is the page-scripting runtime only; it
// is never an extension mechanism (README "JavaScript Execution").
class JavaScriptRuntime {
public:
    virtual ~JavaScriptRuntime() = default;

    virtual ScriptResult evaluate(std::string_view script,
                                  const ScriptOptions& options = {}) = 0;
    virtual ScriptResult run_microtasks(const ScriptOptions& options = {});
    virtual bool has_pending_microtasks() const;
    virtual void set_global(std::string_view name, std::string_view value) = 0;

    // Installs the handler invoked for every console.* call made by page
    // scripts. A session wires it to emit EventType::Console events.
    virtual void set_console_handler(ConsoleHandler handler) = 0;
    virtual void set_document_host(DocumentScriptHost* host);

    virtual std::string name() const = 0;
    virtual CapabilitySet capabilities() const = 0;
};

// Reports JavaScript as unsupported. Used when no JavaScript engine is linked
// into the build so that automation still runs deterministically.
std::unique_ptr<JavaScriptRuntime> make_null_javascript_runtime();

// Returns the QuickJS-backed runtime when ProwseTk is built with
// PROWSETK_HAVE_QUICKJS, otherwise returns the null runtime. The document
// host mediates every DOM, network, and storage operation; when given, the
// web-platform bindings are installed immediately. The host must outlive the
// returned runtime.
std::unique_ptr<JavaScriptRuntime> make_javascript_runtime(
    DocumentScriptHost* host = nullptr);

}  // namespace prowsetk

#endif  // PROWSETK_JAVASCRIPT_RUNTIME_HPP
