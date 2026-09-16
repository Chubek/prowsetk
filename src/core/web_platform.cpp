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

    // Browser JavaScript host bindings. These are the Web API objects that
    // page scripts may reach for. None of them are installed on the page global
    // yet: page scripts execute against a bare QuickJS global. Classifications
    // are deliberately honest about the current state (README "Compatibility
    // Policy"); they become implemented as the bindings are installed.
    platform.declare("window", ImplementationClass::Unsupported,
                     "no JS host binding installed yet");
    platform.declare("document-js", ImplementationClass::Unsupported,
                     "DOM is available through the C++/Lua API, not page JS");
    platform.declare("location", ImplementationClass::Unsupported,
                     "no JS host binding installed yet");
    platform.declare("navigator", ImplementationClass::Unsupported,
                     "no JS host binding installed yet");
    platform.declare("console", ImplementationClass::FullyImplemented,
                     "console.* forwarded as console events");
    platform.declare("URL", ImplementationClass::Unsupported,
                     "no JS host binding installed yet");
    platform.declare("URLSearchParams", ImplementationClass::Unsupported,
                     "no JS host binding installed yet");
    platform.declare("fetch", ImplementationClass::Unsupported,
                     "no JS host binding installed yet");
    platform.declare("XMLHttpRequest", ImplementationClass::Unsupported,
                     "no JS host binding installed yet");
    platform.declare("timers", ImplementationClass::Unsupported,
                     "no JS host binding installed yet");
    platform.declare("cookies", ImplementationClass::Unsupported,
                     "cookie storage exists in C++; not exposed to page JS");
    platform.declare("localStorage", ImplementationClass::Unsupported,
                     "storage exists in C++; not exposed to page JS");
    platform.declare("sessionStorage", ImplementationClass::Unsupported,
                     "storage exists in C++; not exposed to page JS");
    platform.declare("canvas", ImplementationClass::DummyImplementation,
                     "no pixel rendering");
    platform.declare("WebGL", ImplementationClass::Unsupported);
    platform.declare("CSS layout", ImplementationClass::Unsupported,
                     "no layout or rendering engine");
    return platform;
}

}  // namespace prowsetk