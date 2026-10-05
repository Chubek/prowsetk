#include <gtest/gtest.h>
#include <prowsetk/plugins/basic_gui.hpp>
#include <prowsetk/error.hpp>
#include <prowsetk/lua_runtime.hpp>

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

namespace {
struct GuiAgent {
    MemoryNetworkClient transport;
    std::vector<std::string> choices;
    std::size_t replies = 0;
    explicit GuiAgent(std::vector<std::string> values) : choices(std::move(values)) {
        transport.set_handler([this](const HttpRequest& request) -> HttpResponse {
            if (request.url.ends_with("/api/session")) {
                EXPECT_NE(request.body.find("permission"), std::string::npos);
                return {200, {}, R"({"data":{"id":"gui-agent"}})", {}, {}};
            }
            if (request.url.ends_with("/prompt")) {
                ++replies;
                return {200, {}, "{}", {}, {}};
            }
            EXPECT_NE(request.url.find("/message"), std::string::npos);
            if (!replies) return {200, {}, "[]", {}, {}};
            const auto& choice = choices.at(replies - 1);
            const auto body = "{\"data\":[{\"id\":\"reply-" + std::to_string(replies) +
                "\",\"type\":\"assistant\",\"time\":{\"completed\":2},\"content\":[{\"type\":\"text\",\"text\":\"" + choice + "\"}]}]}";
            return {200, {}, body, {}, {}};
        });
    }
};
constexpr auto navigation_marionette = R"lua(
    function main(args)
        assert(session:document())
        session:on('console', function() end)
        return [[{"version":1,"goal":"default goal","max_steps":3,"max_get_probes":0,
          "actions":[{"id":"next","kind":"navigate","url":"/next"}]}]]
    end
)lua";
}

TEST(BasicGuiIntegration, LuaMarionetteUsesOpenCodeV2OnTheDisplayedSession) {
    if (!LuaRuntime::available()) GTEST_SKIP();
    Browser browser;
    auto page = std::make_unique<MemoryNetworkClient>();
    auto* observed = page.get();
    page->set_response("https://app.test/next", HttpResponse{200, {}, "<p>Agent navigated</p>", {}, {}});
    browser.set_network_client(std::move(page));
    auto session = browser.create_session();
    session->set_header("Authorization", "Bearer page-private");
    Controller controller(*session);
    controller.load_html("<p>Initial</p><input value='form-private'><script>var x='script-private'</script>", "https://app.test/start");
    GuiAgent agent({R"({\"action\":\"next\"})", R"({\"action\":\"stop\"})"});
    const auto handlers = browser.events().handler_count(EventType::Console);
    const auto result = controller.run_marionette(navigation_marionette, "GUI goal override", &agent.transport);
    EXPECT_TRUE(result.stopped);
    EXPECT_EQ(result.steps, 1u);
    EXPECT_EQ(result.reason, "agent-stop");
    EXPECT_EQ(session->current_url(), "https://app.test/next");
    EXPECT_NE(controller.snapshot().preview_html.find("Agent navigated"), std::string::npos);
    EXPECT_TRUE(controller.can_back());
    ASSERT_EQ(observed->requests().size(), 1u);
    EXPECT_EQ(browser.events().handler_count(EventType::Console), handlers);
    EXPECT_EQ(browser.events().handler_count(EventType::BeforeRequest), 0u);
    EXPECT_EQ(browser.live_session_count(), 1u);
    bool goal_sent = false;
    for (const auto& request : agent.transport.requests()) {
        goal_sent |= request.body.find("GUI goal override") != std::string::npos;
        EXPECT_EQ(request.body.find("form-private"), std::string::npos);
        EXPECT_EQ(request.body.find("script-private"), std::string::npos);
        for (const auto& header : request.headers) EXPECT_EQ(header.second.find("page-private"), std::string::npos);
    }
    EXPECT_TRUE(goal_sent);
}

TEST(BasicGuiIntegration, MarionetteFailureRefreshesActionsAndReleasesHooks) {
    if (!LuaRuntime::available()) GTEST_SKIP();
    Browser browser;
    auto page = std::make_unique<MemoryNetworkClient>();
    page->set_response("https://app.test/next", HttpResponse{200, {}, "<p>Changed before failure</p>", {}, {}});
    browser.set_network_client(std::move(page));
    auto session = browser.create_session();
    Controller controller(*session);
    controller.load_html("<p>Initial</p>", "https://app.test/start");
    const auto handlers = browser.events().handler_count(EventType::Console);
    GuiAgent agent({R"({\"action\":\"next\"})", "execute arbitrary Lua"});
    EXPECT_THROW(controller.run_marionette(navigation_marionette, {}, &agent.transport), Error);
    EXPECT_NE(controller.snapshot().preview_html.find("Changed before failure"), std::string::npos);
    EXPECT_TRUE(controller.can_back());
    EXPECT_EQ(browser.events().handler_count(EventType::BeforeRequest), 0u);
    EXPECT_EQ(browser.events().handler_count(EventType::Console), handlers);
    // The run guard is released after failure, so another explicit run works.
    GuiAgent retry({R"({\"action\":\"stop\"})"});
    EXPECT_TRUE(controller.run_marionette(navigation_marionette, {}, &retry.transport).stopped);
}

TEST(BasicGuiIntegration, MarionetteRejectsCrossOriginActionsBeforeOpenCode) {
    if (!LuaRuntime::available()) GTEST_SKIP();
    Browser browser;
    auto page = std::make_unique<MemoryNetworkClient>();
    auto* observed = page.get();
    browser.set_network_client(std::move(page));
    auto session = browser.create_session();
    Controller controller(*session);
    controller.load_html("<p>Initial</p>", "https://app.test/start");
    MemoryNetworkClient agent;
    EXPECT_THROW(controller.run_marionette(R"lua(
        function main() return [[{"version":1,"goal":"Explore","max_get_probes":0,
          "actions":[{"id":"outside","kind":"navigate","url":"https://other.test/"}]}]] end
    )lua", {}, &agent), Error);
    EXPECT_TRUE(agent.requests().empty());
    EXPECT_TRUE(observed->requests().empty());
}
