#ifndef PROWSETK_BASIC_GUI_OPENCODE_CONFIG_HPP
#define PROWSETK_BASIC_GUI_OPENCODE_CONFIG_HPP
#include <opencode_bridge.hpp>
#include <string_view>

namespace prowsetk::basic_gui::detail {
// Shared bridge configuration for explicit checks, inquiries and marionettes.
plugins::opencode_bridge::BridgeConfig opencode_config(std::string_view base_url,
                                                     std::size_t max_requests = 512);
}
#endif
