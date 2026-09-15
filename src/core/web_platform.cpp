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

    platform.declare("window", ImplementationClass::PartiallyImplemented,
                     "automation-oriented subset");
    platform.declare("document", ImplementationClass::FullyImplemented);
    platform.declare("location", ImplementationClass::PartiallyImplemented,
                     "read-mostly; navigation is host-driven");
    platform.declare("navigator", ImplementationClass::ImplementedWithRestrictions,
                     "static user-agent and platform fields");
    platform.declare("console", ImplementationClass::FullyImplemented,
                     "forwarded as console events");
    platform.declare("URL", ImplementationClass::FullyImplemented);
    platform.declare("URLSearchParams", ImplementationClass::FullyImplemented);
    platform.declare("fetch", ImplementationClass::ImplementedWithRestrictions,
                     "host-mediated through NetworkClient");
    platform.declare("XMLHttpRequest", ImplementationClass::ImplementedWithRestrictions,
                     "host-mediated through NetworkClient");
    platform.declare("timers", ImplementationClass::PartiallyImplemented);
    platform.declare("cookies", ImplementationClass::ImplementedWithRestrictions,
                     "redaction-aware storage backend");
    platform.declare("localStorage", ImplementationClass::PartiallyImplemented);
    platform.declare("sessionStorage", ImplementationClass::PartiallyImplemented);
    platform.declare("canvas", ImplementationClass::DummyImplementation,
                     "no pixel rendering");
    platform.declare("WebGL", ImplementationClass::Unsupported);
    platform.declare("CSS layout", ImplementationClass::Unsupported,
                     "no layout or rendering engine");
    return platform;
}

}  // namespace prowsetk
