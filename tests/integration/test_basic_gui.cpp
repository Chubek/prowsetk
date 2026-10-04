#include <gtest/gtest.h>
#include <prowsetk/plugins/basic_gui.hpp>
#include <prowsetk/error.hpp>

using namespace prowsetk;
using namespace prowsetk::basic_gui;

TEST(BasicGuiIntegration, NavigationHistoryUsesHostTransportAndCancellation) {
    BrowserConfig config; config.javascript = false;
    Browser browser(config);
    auto transport = std::make_unique<MemoryNetworkClient>();
    transport->set_response("https://app.test/one", HttpResponse{200, {}, "<h1>One</h1><a href='/two'>Next</a>", {}, {}});
    transport->set_response("https://app.test/two", HttpResponse{200, {}, "<h1>Two</h1>", {}, {}});
    browser.set_network_client(std::move(transport));
    auto session = browser.create_session();
    Controller controller(*session);
    controller.navigate("https://app.test/one");
    const auto link = controller.find("a");
    ASSERT_TRUE(link);
    EXPECT_TRUE(controller.click(*link, controller.snapshot().revision));
    ASSERT_TRUE(controller.can_back());
    controller.back();
    EXPECT_EQ(session->current_url(), "https://app.test/one");
    EXPECT_TRUE(controller.can_forward());
    controller.forward();
    EXPECT_EQ(session->current_url(), "https://app.test/two");
    const auto hook = session->events().subscribe(EventType::BeforeNavigation, [](Event& event) { event.cancelled = true; });
    controller.back();
    EXPECT_EQ(session->current_url(), "https://app.test/two");
    EXPECT_TRUE(controller.can_back());
    session->events().unsubscribe(hook);
    controller.load_html("<h1>Offline</h1>");
    session->document()->query_selector("h1")->set_text("Changed");
    controller.reload();
    EXPECT_EQ(session->document()->query_selector("h1")->text(), "Offline");
    EXPECT_THROW(controller.navigate("file:///private"), Error);
}

TEST(BasicGuiIntegration, ScriptActionsMutateDomObserveNetworkAndRejectStaleTargets) {
    if (make_javascript_runtime()->name() == "null") GTEST_SKIP();
    Browser browser;
    auto transport = std::make_unique<MemoryNetworkClient>();
    transport->set_response("https://app.test/api/update?token=private", HttpResponse{200, {}, "{}", {}, {}});
    browser.set_network_client(std::move(transport));
    auto session = browser.create_session();
    Controller controller(*session);
    controller.load_html("<html><body><input id='input'><button id='button'>Update</button><p id='out'></p>"
        "<script>document.querySelector('#button').onclick=()=>{fetch('/api/update?token=private');"
        "Promise.resolve().then(()=>document.querySelector('#out').textContent='Updated');};</script></body></html>", "https://app.test/");
    auto button = controller.find("#button");
    ASSERT_TRUE(button);
    const auto stale_revision = controller.snapshot().revision;
    EXPECT_TRUE(controller.click(*button, stale_revision));
    EXPECT_EQ(session->document()->query_selector("#out")->text(), "Updated");
    ASSERT_EQ(session->page_script_requests().size(), 1u);
    EXPECT_FALSE(controller.click(*button, stale_revision));
    for (const auto& event : controller.activity()) EXPECT_EQ((event.url + event.detail).find("private"), std::string::npos);
    auto input = controller.find("#input");
    ASSERT_TRUE(input);
    EXPECT_TRUE(controller.type(*input, controller.snapshot().revision, "value"));
    EXPECT_EQ(session->document()->query_selector("#input")->value(), "value");
    EXPECT_EQ(controller.snapshot().source_html.find("\"value\""), std::string::npos);
    button = controller.find("#button");
    const auto revision = controller.snapshot().revision;
    session->document()->query_selector("#button")->parent()->remove_child(session->document()->query_selector("#button"));
    EXPECT_FALSE(controller.click(*button, revision));
    controller.load_html("<p>New page</p>");
    EXPECT_FALSE(controller.click(*button, revision));
}

TEST(BasicGuiIntegration, ViewportListenersAndTimersArePumpedThroughSession) {
    if (make_javascript_runtime()->name() == "null") GTEST_SKIP();
    Browser browser;
    auto transport = std::make_unique<MemoryNetworkClient>();
    transport->set_response("https://app.test/resized", HttpResponse{204, {}, {}, {}, {}});
    browser.set_network_client(std::move(transport));
    auto session = browser.create_session();
    Controller controller(*session);
    session->set_viewport({1200, 800, 1});
    controller.load_html("<html><body><p id='state'>Wide</p><script>"
        "var media=matchMedia('(max-width: 700px)');media.onchange=e=>{"
        "document.querySelector('#state').textContent=e.matches?'Narrow':'Wide';fetch('/resized');};"
        "</script></body></html>", "https://app.test/");
    controller.resize_viewport(600, 800);
    EXPECT_EQ(session->document()->query_selector("#state")->text(), "Narrow");
    ASSERT_EQ(session->page_script_requests().size(), 1u);
    EXPECT_EQ(controller.evaluate("setTimeout(()=>document.querySelector('#state').textContent='Timed',0)"),
              "[result hidden; enable Console values to view]");
    EXPECT_EQ(session->document()->query_selector("#state")->text(), "Timed");
    EXPECT_NE(controller.snapshot().preview_html.find("Timed"), std::string::npos);
    bool rejected = false, resize_rejected = false;
    const auto hook = session->events().subscribe(EventType::Console, [&](Event&) {
        try { session->pump_events(); } catch (const Error& e) { rejected = e.code() == ErrorCode::InvalidArgument; }
        try { session->set_viewport({10, 10, 1}); } catch (const Error& e) { resize_rejected = e.code() == ErrorCode::InvalidArgument; }
    });
    session->evaluate_js("console.log('checkpoint')");
    EXPECT_TRUE(rejected);
    EXPECT_TRUE(resize_rejected);
    EXPECT_EQ(session->viewport().width, 600);
    session->events().unsubscribe(hook);
}

TEST(BasicGuiIntegration, NativePluginLoadingIsDisplayAndNetworkFree) {
    Browser browser;
    auto transport = std::make_unique<MemoryNetworkClient>();
    auto* observed = transport.get();
    browser.set_network_client(std::move(transport));
    browser.plugins().load_native(BASIC_GUI_PLUGIN_PATH);
    browser.plugins().initialize_all();
    EXPECT_TRUE(observed->requests().empty());
    auto session = browser.create_session();
    session->load_html("<p>Native facade</p>");
    EXPECT_TRUE(observed->requests().empty());
}
