#ifndef PROWSETK_GUI_PLATFORM_HPP
#define PROWSETK_GUI_PLATFORM_HPP

#include <string_view>

namespace prowsetk::gui {

// Optional native display support used by graphical adapters. These functions
// never affect the headless engine and never create a window.
bool x11_compiled() noexcept;
bool x11_display_available() noexcept;
std::string_view backend_name() noexcept;

} // namespace prowsetk::gui
#endif
