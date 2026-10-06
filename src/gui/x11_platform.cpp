#include <prowsetk/gui_platform.hpp>

#if defined(PROWSETK_GUI_HAVE_X11)
#include <X11/Xlib.h>
#endif

namespace prowsetk::gui {
bool x11_compiled() noexcept {
#if defined(PROWSETK_GUI_HAVE_X11)
    return true;
#else
    return false;
#endif
}
bool x11_display_available() noexcept {
#if defined(PROWSETK_GUI_HAVE_X11)
    Display* display = XOpenDisplay(nullptr);
    if (!display) return false;
    XCloseDisplay(display);
    return true;
#else
    return false;
#endif
}
std::string_view backend_name() noexcept {
#if defined(PROWSETK_GUI_HAVE_X11)
    return "X11";
#else
    return "headless";
#endif
}
} // namespace prowsetk::gui
