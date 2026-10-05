#include <gtest/gtest.h>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <string>
#include <FL/Fl.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Window.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Help_View.H>
#include <FL/Fl_Text_Editor.H>
#include <prowsetk/lua_runtime.hpp>
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

// The launcher script is a required part of the shipped workflow: it must work
// with no display, and its preflight checks must be trustworthy.
TEST(BasicGuiWindow, LauncherScriptHelpAndPreflightChecksWork) {
    const std::string script = std::string(PROWSETK_SOURCE_DIR) + "/tools/launch-gui.sh";
    ASSERT_NE(std::fopen(script.c_str(), "r"), nullptr);

    // The script is single-quoted throughout; these fixture values cannot
    // terminate the quote.
    const auto run = [&script](std::initializer_list<std::string> args) {
        std::string command = "bash '" + script + "'";
        for (const auto& argument : args) {
            // A single quote would terminate the shell quoting, so reject it
            // rather than letting a fixture inject shell syntax.
            EXPECT_EQ(argument.find('\''), std::string::npos) << "unsafe fixture";
            command += " '" + argument + "'";
        }
        command += " 2>&1";
        std::string output;
        std::FILE* pipe = popen(command.c_str(), "r");
        if (pipe == nullptr) return output;
        char buffer[512];
        while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) output += buffer;
        pclose(pipe);
        return output;
    };

    // Help never needs a build or a display, and documents the build step.
    const auto help = run({"--help"});
    EXPECT_NE(help.find("--url"), std::string::npos);
    EXPECT_NE(help.find("--file"), std::string::npos);
    EXPECT_NE(help.find("--proxy"), std::string::npos);
    EXPECT_NE(help.find("cmake --build --preset gui"), std::string::npos);
    EXPECT_NE(help.find("PROWSETK_GUI_BIN"), std::string::npos);
    // Preconditions fail loudly instead of launching a broken window.
    EXPECT_NE(run({"--url", "https://app.test", "--file", "x.html"}).find("not both"), std::string::npos);
    EXPECT_NE(run({"--url", "file:///etc/passwd"}).find("http:// or https://"), std::string::npos);
    EXPECT_NE(run({"--file", "/nonexistent-page.html"}).find("not readable"), std::string::npos);
    EXPECT_NE(run({"--bin", "/nonexistent/ptk-basic-gui"}).find("not executable"), std::string::npos);
    // A proxy URL with userinfo is refused so credentials never reach `ps`.
    EXPECT_NE(run({"--url", "https://app.test", "--proxy",
                   "http://user:secret@proxy.test:8080"}).find("credentials"),
              std::string::npos);
    // Unknown options are forwarded to the binary, which owns their diagnostics.
    EXPECT_EQ(run({"--unknown-option"}).find("invalid arguments"), std::string::npos);
    // A no-display launch is reported, not silently ignored.
    const auto display = std::getenv("DISPLAY");
    if (display == nullptr || *display == '\0') {
        EXPECT_NE(run({"--file", "/dev/null"}).find("no display"), std::string::npos);
    }
}

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
        viewer.set_opencode_base_url("http://127.0.0.1:4096");
        viewer.show();
        Fl::check();
        {
            auto* window = Fl::first_window();
            ASSERT_NE(window, nullptr);
            auto* server = dynamic_cast<Fl_Input*>(named_widget(*window, "OpenCode URL"));
            auto* check = named_widget(*window, "Check OpenCode");
            auto* ask = named_widget(*window, "Ask OpenCode");
            ASSERT_NE(server, nullptr); ASSERT_NE(check, nullptr); ASSERT_NE(ask, nullptr);
            EXPECT_STREQ(server->value(), "http://127.0.0.1:4096");
            // URL validation runs before any live server request.
            server->value("file:///private"); check->do_callback();
            EXPECT_TRUE(observed->requests().empty());
            server->value("http://127.0.0.1:4096");
        }
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
        if (prowsetk::LuaRuntime::available()) {
            auto* window = Fl::first_window();
            ASSERT_NE(window, nullptr);
            auto* editor = first_of_type<Fl_Text_Editor>(*window);
            auto* run = named_widget(*window, "Run marionette");
            auto* goal = dynamic_cast<Fl_Input*>(named_widget(*window, "OpenCode goal"));
            ASSERT_NE(editor, nullptr); ASSERT_NE(run, nullptr); ASSERT_NE(goal, nullptr);
            // Exercise the real GUI callback without any OpenCode request.
            // Lua preparation takes effect before strict policy rejection.
            editor->buffer()->text("function main(args) assert(args.goal == 'Test goal'); "
                "session:document():query_selector('#state'):set_text('Lua prepared'); return '{}' end");
            goal->value("Test goal");
            run->do_callback();
            EXPECT_EQ(session->document()->query_selector("#state")->text(), "Lua prepared");
            auto* preview = first_of_type<Fl_Help_View>(*window);
            ASSERT_NE(preview, nullptr);
            EXPECT_NE(std::strstr(preview->value(), "Lua prepared"), nullptr);
            EXPECT_TRUE(observed->requests().empty());
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
