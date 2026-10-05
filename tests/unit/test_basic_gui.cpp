#include <gtest/gtest.h>
#include <prowsetk/plugins/basic_gui.hpp>
#include <prowsetk/error.hpp>
#include <prowsetk/ir.hpp>
#include <prowsetk/lua_runtime.hpp>
#include <fstream>

using namespace prowsetk;
using namespace prowsetk::basic_gui;

TEST(BasicGui, ProjectionPreservesCanonicalPathsAndNeverLoadsResources) {
    auto document = parse_html("<main><h1>Heading</h1><a href='/next'>Next</a>"
        "<img src='file:///private/image.png' alt='picture'><button>Act</button></main>");
    auto snapshot = inspect_document(*document, 42);
    ASSERT_FALSE(snapshot.limited);
    std::vector<std::string> paths;
    for (const auto& event : emit_prowse_events(*document)) if (event.kind == "start") paths.push_back(event.xpath);
    ASSERT_EQ(snapshot.nodes.size(), paths.size());
    for (std::size_t i = 0; i < paths.size(); ++i) EXPECT_EQ(snapshot.nodes[i].path, paths[i]);
    EXPECT_NE(snapshot.preview_html.find("Heading"), std::string::npos);
    EXPECT_NE(snapshot.preview_html.find("prowse-action:42:"), std::string::npos);
    EXPECT_EQ(snapshot.preview_html.find("file:"), std::string::npos);
    EXPECT_EQ(snapshot.preview_html.find("<img"), std::string::npos);
    EXPECT_EQ(snapshot.preview_html.find("href='/next'"), std::string::npos);
}

TEST(BasicGui, SnapshotsRedactFormsScriptsAndSensitiveAttributes) {
    auto document = parse_html("<html><body><input value='private-input'>"
        "<textarea>private-textarea</textarea><select><option value='private-option'>private-choice</option></select>"
        "<script>private-script</script><style>private-style</style>"
        "<p data-token='private-token' onclick='private-handler'>Public</p>"
        "<a href='https://u:p@site.test/a?token=private-query#private-fragment'>Next</a></body></html>");
    const auto before = document->html();
    const auto snapshot = inspect_document(*document);
    const auto output = snapshot.source_html + snapshot.preview_html + snapshot.url;
    EXPECT_EQ(output.find("private-"), std::string::npos);
    EXPECT_EQ(output.find("u:p@"), std::string::npos);
    EXPECT_NE(output.find("Public"), std::string::npos);
    EXPECT_NE(output.find("[REDACTED]"), std::string::npos);
    EXPECT_EQ(document->html(), before);
}

TEST(BasicGui, VisibilityAndSnapshotBoundsAreExplicit) {
    auto document = parse_html("<main><div hidden><b>hidden-text</b></div>"
        "<details><summary>Summary</summary><p>collapsed-text</p></details>"
        "<dialog>closed-dialog</dialog><p style='display: none'>style-hidden</p></main>");
    auto snapshot = inspect_document(*document);
    EXPECT_NE(snapshot.preview_html.find("Summary"), std::string::npos);
    for (auto text : {"hidden-text", "collapsed-text", "closed-dialog", "style-hidden"}) {
        EXPECT_EQ(snapshot.preview_html.find(text), std::string::npos);
        EXPECT_NE(snapshot.source_html.find(text), std::string::npos);
    }
    auto huge = parse_html("<p>" + std::string(max_snapshot_bytes, 'x') + "</p>");
    EXPECT_TRUE(inspect_document(*huge).limited);
    std::string many;
    for (std::size_t i = 0; i <= max_snapshot_nodes; ++i) many += "<b></b>";
    EXPECT_TRUE(inspect_document(*parse_html(many)).limited);
    std::string deep;
    for (std::size_t i = 0; i < max_snapshot_depth + 2; ++i) deep += "<div>";
    EXPECT_TRUE(inspect_document(*parse_html(deep)).limited);
}

TEST(BasicGui, AnAlreadyLoadedPageIsInspectableImmediately) {
    Browser browser;
    auto session = browser.create_session();
    Controller empty(*session);
    EXPECT_TRUE(empty.snapshot().nodes.empty());
    session->load_html("<h1 id='ready'>Ready</h1>");
    Controller controller(*session);
    EXPECT_NE(controller.snapshot().preview_html.find("Ready"), std::string::npos);
    const auto ready = controller.find("#ready");
    ASSERT_TRUE(ready);
    EXPECT_EQ(controller.snapshot().nodes[*ready].path, "/h1[1]");
}

TEST(BasicGui, ConsoleIsOptInBoundedAndSessionScoped) {
    Browser browser;
    auto session = browser.create_session();
    const auto count = browser.events().handler_count(EventType::Console);
    {
        Controller controller(*session);
        Event event;
        event.type = EventType::Console;
        event.name = "log";
        event.message = "private-console";
        event.url = "https://user:pass@site.test/path?q=private-query#private-fragment";
        event.session_id = "another-session";
        browser.events().emit(event);
        EXPECT_TRUE(controller.activity().empty());
        event.session_id = session->id();
        browser.events().emit(event);
        ASSERT_EQ(controller.activity().size(), 1u);
        EXPECT_EQ(controller.activity().back().url, "https://site.test/path");
        EXPECT_EQ(controller.activity().back().detail.find("private-console"), std::string::npos);
        controller.show_console_values(true);
        browser.events().emit(event);
        EXPECT_NE(controller.activity().back().detail.find("private-console"), std::string::npos);
        for (std::size_t i = 0; i < max_activity_entries; ++i) browser.events().emit(event);
        EXPECT_EQ(controller.activity().size(), max_activity_entries);
        EXPECT_EQ(controller.dropped_activity(), 2u);
        controller.clear_activity();
        EXPECT_TRUE(controller.activity().empty());
    }
    EXPECT_EQ(browser.events().handler_count(EventType::Console), count);
}

TEST(BasicGui, DisabledViewerReportsUnsupported) {
    if (available()) GTEST_SKIP();
    Browser browser;
    auto session = browser.create_session();
    try { Viewer viewer(*session); FAIL(); }
    catch (const Error& error) { EXPECT_EQ(error.code(), ErrorCode::Unsupported); }
}

TEST(BasicGui, MarionetteRejectsBadScriptsAndPoliciesWithoutContactingAgent) {
    if (!LuaRuntime::available()) GTEST_SKIP();
    Browser browser;
    auto session = browser.create_session();
    Controller controller(*session);
    controller.load_html("<p id='state'>Initial</p>");
    MemoryNetworkClient agent;
    for (const auto source : {"not valid Lua !", "function main() error('private-error') end",
                              "function main() return '{}' end", "function main() return 0 end",
                              "function main() return setmetatable({}, {__tostring=function() error('private-error') end}) end"}) {
        try { controller.run_marionette(source, {}, &agent); FAIL(); }
        catch (const Error& error) { EXPECT_EQ(std::string(error.what()).find("private-error"), std::string::npos); }
    }
    EXPECT_TRUE(agent.requests().empty());
    EXPECT_THROW(controller.run_marionette(std::string(65537, 'x'), {}, &agent), Error);
    EXPECT_THROW(controller.run_marionette("", std::string(4097, 'x'), &agent), Error);
    EXPECT_TRUE(agent.requests().empty());
    // A failure after Lua preparation still updates the inspected live DOM.
    EXPECT_THROW(controller.run_marionette(R"lua(
        function main()
            session:document():query_selector('#state'):set_text('Prepared')
            return '{}'
        end
    )lua", {}, &agent), Error);
    EXPECT_NE(controller.snapshot().preview_html.find("Prepared"), std::string::npos);
    EXPECT_EQ(browser.live_session_count(), 1u);
}

TEST(BasicGui, MarionetteFilesAreBoundedAndErrorsOmitPaths) {
    Browser browser;
    auto session = browser.create_session();
    Controller controller(*session);
    const auto file = std::filesystem::current_path() / "basic-gui-marionette-limit.lua";
    { std::ofstream stream(file, std::ios::binary); stream << std::string(65537, 'x'); }
    try { controller.run_marionette_file(file); FAIL(); }
    catch (const Error& error) { EXPECT_EQ(error.code(), ErrorCode::ResourceLimit); }
    std::filesystem::remove(file);
    try { controller.run_marionette_file(file); FAIL(); }
    catch (const Error& error) {
        EXPECT_EQ(error.code(), ErrorCode::IoError);
        EXPECT_EQ(std::string(error.what()).find(file.string()), std::string::npos);
    }
}
