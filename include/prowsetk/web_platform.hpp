#ifndef PROWSETK_WEB_PLATFORM_HPP
#define PROWSETK_WEB_PLATFORM_HPP

#include <string>
#include <string_view>

#include "prowsetk/capability.hpp"

namespace prowsetk {

// Describes the browser-compatible Web APIs Flatworm exposes, each carrying an
// implementation classification (README "Compatibility Policy").
class WebPlatform {
public:
    WebPlatform();

    void declare(std::string name, ImplementationClass classification,
                 std::string notes = {});

    bool supports(std::string_view api) const noexcept;
    ImplementationClass classification(std::string_view api) const noexcept;
    const CapabilitySet& capabilities() const noexcept { return capabilities_; }

private:
    CapabilitySet capabilities_;
};

// The default, documented capability surface of a stock ProwseTk build.
WebPlatform default_web_platform();

}  // namespace prowsetk

#endif  // PROWSETK_WEB_PLATFORM_HPP
