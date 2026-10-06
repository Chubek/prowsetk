// Build-independent availability query.
//
// The GUI-enabled adapter lives in the plugin shared library and needs no
// descriptor of its own: the model library reports the configuration it was
// built with, so an external consumer linking only
// `ProwseTk::basic_gui_model` can still ask whether the FLTK adapter is usable
// without taking an FLTK dependency.
#include <prowsetk/plugins/basic_gui.hpp>

namespace prowsetk::basic_gui {
bool available() noexcept {
#ifdef PROWSETK_HAVE_BASIC_GUI
    return true;
#else
    return false;
#endif
}
bool display_available() noexcept { return available() && gui::x11_display_available(); }
}  // namespace prowsetk::basic_gui
