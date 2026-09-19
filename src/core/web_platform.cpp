#include "prowsetk/web_platform.hpp"

namespace prowsetk {

WebPlatform::WebPlatform() = default;

void WebPlatform::declare(std::string name,
                          ImplementationClass classification,
                          std::string notes) {
    capabilities_.set(std::move(name), classification, std::move(notes));
}

bool WebPlatform::supports(std::string_view api) const noexcept {
    return capabilities_.has(api);
}

ImplementationClass WebPlatform::classification(
    std::string_view api) const noexcept {
    return capabilities_.classification(api);
}

WebPlatform default_web_platform() {
    WebPlatform platform;

    // The C++ engine surfaces. These are real and usable through the public C++
    // API, the Lua layer, and the web interface.
    platform.declare("dom", ImplementationClass::FullyImplemented,
                     "tolerant HTML parser and queryable DOM tree");
    platform.declare("css-selectors", ImplementationClass::PartiallyImplemented,
                     "type, class, id, attribute, and structural pseudos");
    platform.declare("xpath", ImplementationClass::PartiallyImplemented,
                     "XPath 1.0 through the vendored pugixml engine");
    platform.declare("network", ImplementationClass::ImplementedWithRestrictions,
                     "host-mediated; plain HTTP over sockets by default");
    platform.declare("storage", ImplementationClass::FullyImplemented,
                     "in-memory cookies, local and session storage");
    platform.declare("events", ImplementationClass::FullyImplemented,
                     "navigation, request, document, script, and lifecycle");
    platform.declare("endpoint-extraction",
                     ImplementationClass::PartiallyImplemented,
                     "heuristic; provenance and confidence preserved");
    platform.declare("javascript", ImplementationClass::PartiallyImplemented,
                     "QuickJS page-scripting runtime when linked");

    // Browser JavaScript host bindings. These are the Web API objects page
    // scripts reach for; they are installed on the page global by the
    // Flatworm web platform shim (src/core/web_platform_shim.hpp) over the
    // host-mediated `DocumentScriptHost` primitives. Classifications are
    // deliberately honest about the current state (README "Compatibility
    // Policy").
    platform.declare("window", ImplementationClass::PartiallyImplemented,
                     "window/self/top/parent aliases, listeners, timers, storage");
    platform.declare("document-js", ImplementationClass::PartiallyImplemented,
                     "handle-based DOM bridge: query(Selector|SelectorAll), "
                     "createElement, attributes, textContent, innerHTML, mutation");
    platform.declare("location", ImplementationClass::PartiallyImplemented,
                     "reads resolve against the live document; assignment and "
                     "form submit trigger a host navigation after the script pass");
    platform.declare("navigator", ImplementationClass::PartiallyImplemented,
                     "static fields from the session configuration");
    platform.declare("console", ImplementationClass::FullyImplemented,
                     "console.* forwarded as console events");
    platform.declare("URL", ImplementationClass::PartiallyImplemented,
                     "polyfill over the engine URL parser; no blob/data handling");
    platform.declare("URLSearchParams", ImplementationClass::PartiallyImplemented,
                     "polyfill; string-backed pairs");
    platform.declare("fetch", ImplementationClass::ImplementedWithRestrictions,
                     "host-mediated NetworkClient; microtask delivery; no streaming bodies");
    platform.declare("XMLHttpRequest", ImplementationClass::ImplementedWithRestrictions,
                     "host-mediated NetworkClient; sync and async; no progress, upload, or CORS policy");
    platform.declare("timers", ImplementationClass::PartiallyImplemented,
                     "setTimeout/setInterval/rAF drained by bounded flush passes after document scripts");
    platform.declare("cookies", ImplementationClass::PartiallyImplemented,
                     "document.cookie through the session cookie jar");
    platform.declare("localStorage", ImplementationClass::PartiallyImplemented,
                     "string API over the session storage backend");
    platform.declare("sessionStorage", ImplementationClass::PartiallyImplemented,
                     "string API over the session storage backend");
    platform.declare("canvas", ImplementationClass::DummyImplementation,
                     "no pixel rendering");
    platform.declare("WebGL", ImplementationClass::Unsupported);
    platform.declare("CSS layout", ImplementationClass::Unsupported,
                     "no layout or rendering engine");
    return platform;
}

}  // namespace prowsetk