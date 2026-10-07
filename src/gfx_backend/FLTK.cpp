#include "FLTK-GUI.hpp"
#include "backend_internal.hpp"
#include <FL/Fl.H>
#include <FL/Fl_Double_Window.H>
#include <FL/fl_draw.H>
#include <algorithm>
#include <cstdlib>
#ifdef PROWSETK_GFX_FLTK_X11
#include <X11/Xlib.h>
#endif

namespace prowsetk::gfx {
namespace {
class Preview final : public Fl_Double_Window {
public:
    explicit Preview(const GfxFrame& frame)
        : Fl_Double_Window(static_cast<int>(frame.surface.width),
                           static_cast<int>(frame.surface.height), "ProwseTk event preview"), frame_(frame) {}
    void draw() override {
        fl_push_clip(0, 0, w(), h());
        fl_color(FL_WHITE); fl_rectf(0, 0, w(), h());
        fl_color(FL_BLACK); fl_font(FL_COURIER, 14);
        for (const auto& text : frame_.text) {
            const int y = static_cast<int>(text.y) - scroll_;
            if (y >= -18 && y <= h()) fl_draw(text.text.data(), static_cast<int>(text.text.size()),
                                             static_cast<int>(text.x), y + 14);
        }
        fl_pop_clip();
    }
    int handle(int event) override {
        if (event == FL_MOUSEWHEEL) {
            scroll_ = std::clamp(scroll_ + Fl::event_dy() * 54, 0, static_cast<int>(max_gfx_content_height));
            redraw(); return 1;
        }
        if (event == FL_KEYDOWN && (Fl::event_key() == FL_Escape || Fl::event_key() == 'q')) {
            hide(); return 1;
        }
        return Fl_Double_Window::handle(event);
    }
private:
    const GfxFrame& frame_;
    int scroll_ = 0;
};
ProwseGfxStatus run(void* instance) noexcept {
    if (!instance) return PROWSETK_GFX_INVALID_ARGUMENT;
#ifdef PROWSETK_GFX_FLTK_X11
    // FLTK's display failure terminates the process. Preflight its X11 build.
    Display* display = XOpenDisplay(nullptr);
    if (!display) return PROWSETK_GFX_UNAVAILABLE;
    XCloseDisplay(display);
#endif
    try {
        Preview window(static_cast<State*>(instance)->frame);
        window.end(); window.show();
        while (window.shown()) Fl::wait();
        return PROWSETK_GFX_OK;
    } catch (...) { return PROWSETK_GFX_ERROR; }
}
}
const ProwseGfxBackend& fltk_backend() {
    static const ProwseGfxBackend definition{PROWSETK_GFX_ABI_VERSION,
        sizeof(ProwseGfxBackend), "fltk", create, destroy, submit, run};
    return definition;
}
}
