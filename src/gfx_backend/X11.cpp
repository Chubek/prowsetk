#include "X11-GUI.hpp"
#include "backend_internal.hpp"
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <algorithm>

namespace prowsetk::gfx {
namespace {
struct DisplayCloser { void operator()(Display* display) const { XCloseDisplay(display); } };
struct WindowOwner {
    Display* display;
    Window window;
    GC gc;
    ~WindowOwner() { if (gc) XFreeGC(display, gc); XDestroyWindow(display, window); }
};
ProwseGfxStatus run(void* instance) noexcept {
    if (!instance) return PROWSETK_GFX_INVALID_ARGUMENT;
    try {
        const auto& frame = static_cast<State*>(instance)->frame;
        const std::unique_ptr<Display, DisplayCloser> display(XOpenDisplay(nullptr));
        if (!display) return PROWSETK_GFX_UNAVAILABLE;
        const int screen = DefaultScreen(display.get());
        const auto window = XCreateSimpleWindow(display.get(), RootWindow(display.get(), screen),
            0, 0, frame.surface.width, frame.surface.height, 0,
            BlackPixel(display.get(), screen), WhitePixel(display.get(), screen));
        if (!window) return PROWSETK_GFX_ERROR;
        WindowOwner owner{display.get(), window, XCreateGC(display.get(), window, 0, nullptr)};
        if (!owner.gc) return PROWSETK_GFX_ERROR;
        XStoreName(display.get(), window, "ProwseTk event preview");
        XSelectInput(display.get(), window, ExposureMask | KeyPressMask | ButtonPressMask | StructureNotifyMask);
        const Atom close = XInternAtom(display.get(), "WM_DELETE_WINDOW", False);
        Atom protocol = close;
        XSetWMProtocols(display.get(), window, &protocol, 1);
        XMapWindow(display.get(), window);
        int scroll = 0;
        int height = static_cast<int>(frame.surface.height);
        const auto draw = [&]() {
            XClearWindow(display.get(), window);
            XSetForeground(display.get(), owner.gc, BlackPixel(display.get(), screen));
            for (const auto& text : frame.text) {
                const int y = static_cast<int>(text.y) - scroll;
                if (y < -18 || y > height) continue;
                XDrawString(display.get(), window, owner.gc, static_cast<int>(text.x), y + 14,
                    text.text.data(), static_cast<int>(text.text.size()));
            }
            XFlush(display.get());
        };
        bool open = true;
        while (open) {
            XEvent event{}; XNextEvent(display.get(), &event);
            if (event.type == Expose && event.xexpose.count == 0) draw();
            else if (event.type == ConfigureNotify) { height = event.xconfigure.height; draw(); }
            else if (event.type == ClientMessage && static_cast<Atom>(event.xclient.data.l[0]) == close) open = false;
            else if (event.type == KeyPress) {
                const auto key = XLookupKeysym(&event.xkey, 0);
                if (key == XK_Escape || key == XK_q) open = false;
                else if (key == XK_Down || key == XK_Page_Down) scroll += key == XK_Down ? 18 : height;
                else if (key == XK_Up || key == XK_Page_Up) scroll -= key == XK_Up ? 18 : height;
                scroll = std::clamp(scroll, 0, static_cast<int>(max_gfx_content_height)); draw();
            } else if (event.type == ButtonPress) {
                if (event.xbutton.button == 4) scroll -= 54;
                if (event.xbutton.button == 5) scroll += 54;
                scroll = std::clamp(scroll, 0, static_cast<int>(max_gfx_content_height)); draw();
            }
        }
        return PROWSETK_GFX_OK;
    } catch (...) { return PROWSETK_GFX_ERROR; }
}
}
const ProwseGfxBackend& x11_backend() {
    static const ProwseGfxBackend definition{PROWSETK_GFX_ABI_VERSION,
        sizeof(ProwseGfxBackend), "x11", create, destroy, submit, run};
    return definition;
}
}
