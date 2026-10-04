#include <gtest/gtest.h>
#include <cstdlib>
#include <FL/Fl.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Window.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Help_View.H>
#include <cstring>
#include <prowsetk/plugins/basic_gui.hpp>

namespace {
Fl_Widget* named_widget(Fl_Group& group, const char* label) {
    for (int i = 0; i < group.children(); ++i) {
        auto* widget = group.child(i);
        if (widget->label() && std::strcmp(widget->label(), label) == 0) return widget;
        if (auto* child = dynamic_cast<Fl_Group*>(widget)) {
            if (auto* found = named_widget(*child, label)) return found;
        }
    }
    return nullptr;
}
// Unlabeled widgets (the page preview) are found by type.
template <typename T>
T* first_of_type(Fl_Group& group) {
    for (int i = 0; i < group.children(); ++i) {
        if (auto* match = dynamic_cast<T*>(group.child(i))) return match;
        if (auto* child = dynamic_cast<Fl_Group*>(group.child(i))) {
            if (auto* found = first_of_type<T>(*child)) return found;
        }
    }
    return nullptr;
}
}  // namespace

TEST(BasicGuiWindow, LiveWindowRefreshAndClosureRetainSessionOwnership) {
#if defined(__linux__)
    if (!std::getenv("DISPLAY")) GTEST_SKIP() << "No display or xvfb-run supplied";
#endif
    prowsetk::Browser browser;
    auto transport = std::make_unique<prowsetk::MemoryNetworkClient>();
    auto* observed = transport.get();
    transport->set_response("https://app.test/page", prowsetk::HttpResponse{200, {}, "<p>Live</p>", {}, {}});
    browser.set_network_client(std::move(transport));
    auto session = browser.create_session();
    {
        prowsetk::basic_gui::Viewer viewer(*session);
        viewer.load_html("<html><body><p id='state'>Initial</p><img src='file:///unavailable'>"
            "<input id='input'><button id='button' onclick=\"document.querySelector('#state').textContent='Clicked'\">Click me</button>"
            "</body></html>");
        EXPECT_TRUE(observed->requests().empty());
        viewer.show();
        Fl::check();
        if (prowsetk::make_javascript_runtime()->name() != "null") {
            auto* window = Fl::first_window();
            ASSERT_NE(window, nullptr);
            auto* selector = dynamic_cast<Fl_Input*>(named_widget(*window, "CSS selector"));
            auto* click = dynamic_cast<Fl_Button*>(named_widget(*window, "Click"));
            auto* input = dynamic_cast<Fl_Input*>(named_widget(*window, "Type text"));
            auto* type = dynamic_cast<Fl_Button*>(named_widget(*window, "Type / append"));
            ASSERT_NE(selector, nullptr); ASSERT_NE(click, nullptr);
            ASSERT_NE(input, nullptr); ASSERT_NE(type, nullptr);
            selector->value("#button"); click->do_callback();
            EXPECT_EQ(session->document()->query_selector("#state")->text(), "Clicked");
            // The rendered preview carries action IDs and never page resource URLs.
            auto* page = first_of_type<Fl_Help_View>(*window);
            ASSERT_NE(page, nullptr);
            ASSERT_NE(page->value(), nullptr);
            EXPECT_NE(std::strstr(page->value(), "prowse-action:"), nullptr);
            EXPECT_EQ(std::strstr(page->value(), "file:"), nullptr);
            EXPECT_EQ(std::strstr(page->value(), "<img"), nullptr);
            selector->value("#input"); input->value("private-typed"); type->do_callback();
            EXPECT_EQ(session->document()->query_selector("#input")->value(), "private-typed");
            EXPECT_STREQ(input->value(), "");
        }
        viewer.refresh();
        Fl::check();
        viewer.close();
        viewer.navigate("https://app.test/page");
        viewer.show();
        Fl::check();
        viewer.close();
    }
    EXPECT_EQ(observed->requests().size(), 1u);
    EXPECT_EQ(session->document()->text(), "Live");
    EXPECT_EQ(browser.live_session_count(), 1u);
}
