#include <gtest/gtest.h>
#include <prowsetk/plugins/complex_gui.hpp>
#include <FL/Fl.H>
#include <cstdlib>
using namespace prowsetk;
TEST(ComplexGuiWindow, PaintsResizesAndCloses) {
    if (!std::getenv("DISPLAY") && !std::getenv("WAYLAND_DISPLAY")) GTEST_SKIP();
    Browser browser; browser.set_network_client(std::make_unique<MemoryNetworkClient>());
    auto session=browser.create_session();
    complex_gui::Viewer viewer(*session);
    viewer.controller().load_html("<body><h1>Canvas test</h1><button>Click</button><input><div style='height:1200px;background-color:blue'>Scroll</div></body>");
    viewer.show(); Fl::check(); viewer.refresh(); Fl::flush();
    EXPECT_FALSE(viewer.controller().page().paint.empty());
    EXPECT_GT(viewer.controller().page().content_height,1000);
    viewer.hide(); Fl::check();
}
