#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "prowsetk/browser.hpp"
#include "prowsetk/error.hpp"
#include "prowsetk/lua_runtime.hpp"

namespace {

class FlatwormModuleSessions : public ::testing::Test {
protected:
    void SetUp() override {
        if (prowsetk::make_javascript_runtime()->name() == "null") GTEST_SKIP();
        auto transport = std::make_unique<prowsetk::MemoryNetworkClient>();
        network = transport.get();
        browser.set_network_client(std::move(transport));
    }
    prowsetk::Browser browser;
    prowsetk::MemoryNetworkClient* network = nullptr;
};

TEST_F(FlatwormModuleSessions, NativeBindingsMutatePagesWithIsolatedPersistentInstanceState) {
    browser.modules().load_native(TEST_MODULE_PATH);
    auto first = browser.create_session();
    auto second = browser.create_session();
    const char* page = "<p id='value'></p><script>"
        "document.getElementById('value').textContent = Flatworm.module('fixture').next();</script>";
    first->load_html(page, "https://example.test/first");
    second->load_html(page, "https://example.test/second");
    EXPECT_EQ(first->document()->query_selector("#value")->text(), "1");
    EXPECT_EQ(second->document()->query_selector("#value")->text(), "1");
    first->load_html(page, "https://example.test/again");
    EXPECT_EQ(first->document()->query_selector("#value")->text(), "2");
    EXPECT_EQ(second->evaluate_js("Flatworm.module('fixture').next()"), "2");
    EXPECT_TRUE(network->requests().empty());
    EXPECT_TRUE(browser.plugins().plugins().empty());
}

TEST_F(FlatwormModuleSessions, InlineAndExternalModuleScriptsUseHostMediatedRequests) {
    browser.modules().load_native(TEST_MODULE_PATH);
    prowsetk::HttpResponse page;
    page.status = 200;
    page.body = "<p id='value'></p>"
        "<script type='module'>import {echo} from 'flatworm:fixture'; "
        "document.getElementById('value').textContent = echo('inline');</script>"
        "<script type='module' src='/entry.js'></script>";
    network->set_response("https://example.test/", page);
    prowsetk::HttpResponse script;
    script.status = 200;
    script.body = "import {echo} from 'flatworm:fixture'; "
        "fetch(echo('/api/value')).then(r => r.text()).then(t => "
        "document.getElementById('value').textContent += echo(':' + t));";
    script.headers.emplace_back("Content-Type", "application/javascript");
    network->set_response("https://example.test/entry.js", script);
    prowsetk::HttpResponse response;
    response.status = 200;
    response.body = "host-mediated";
    network->set_response("https://example.test/api/value", response);
    std::vector<prowsetk::Event> failures;
    browser.events().subscribe(prowsetk::EventType::ScriptException,
        [&](const prowsetk::Event& event) { failures.push_back(event); });
    auto session = browser.create_session();
    session->navigate("https://example.test/");
    EXPECT_TRUE(failures.empty());
    EXPECT_EQ(session->document()->query_selector("#value")->text(), "inline:host-mediated");
    ASSERT_EQ(network->requests().size(), 3u);
    EXPECT_EQ(network->requests()[1].url, "https://example.test/entry.js");
    EXPECT_EQ(network->requests()[2].url, "https://example.test/api/value");
    ASSERT_EQ(session->page_script_requests().size(), 1u);
    EXPECT_EQ(session->page_script_requests()[0].url, "https://example.test/api/value");
}

TEST_F(FlatwormModuleSessions, ImportPolicyErrorsPreserveDocumentAndOmitSpecifierValues) {
    browser.modules().load_native(TEST_MODULE_PATH);
    std::vector<std::string> errors;
    browser.events().subscribe(prowsetk::EventType::ScriptException,
        [&](const prowsetk::Event& event) { errors.push_back(event.message); });
    auto session = browser.create_session();
    session->load_html("<p>retained</p><script type='module'>"
        "import 'https://example.test/private-import-marker.js';</script>"
        "<script type='module'>import '/private-import-marker.js';</script>"
        "<script>document.querySelector('p').textContent += Flatworm.module('fixture').echo(':ok');</script>",
        "https://example.test/");
    EXPECT_EQ(session->document()->query_selector("p")->text(), "retained:ok");
    ASSERT_EQ(errors.size(), 2u);
    for (const auto& error : errors) EXPECT_EQ(error.find("private-import-marker"), std::string::npos);
    EXPECT_TRUE(network->requests().empty());
}

TEST_F(FlatwormModuleSessions, RegistryChangesAffectFutureContextsAndRetainCapturedNativeFunctions) {
    auto before = browser.create_session();
    auto library = browser.modules().load_native(TEST_MODULE_PATH);
    EXPECT_THROW(before->evaluate_js("Flatworm.module('fixture')"), prowsetk::Error);
    auto installed = browser.create_session();
    EXPECT_EQ(installed->evaluate_js("globalThis.saved = Flatworm.module('fixture').next; saved()"), "1");
    EXPECT_TRUE(browser.modules().remove("fixture"));
    library.reset();
    EXPECT_EQ(installed->evaluate_js("saved()"), "2");
    auto after = browser.create_session();
    EXPECT_THROW(after->evaluate_js("Flatworm.module('fixture')"), prowsetk::Error);
    installed.reset();
    EXPECT_EQ(browser.live_session_count(), 2u);
}

int initialized = 0;
int shut_down = 0;
bool fail_initialize = false;
FlatwormStatus initialize(const FlatwormModuleHostApi*, void**) {
    ++initialized;
    return fail_initialize ? FLATWORM_STATUS_ERROR : FLATWORM_STATUS_OK;
}
void shutdown(void*) { ++shut_down; }
FlatwormModuleDefinition lifecycle_module() {
    return {FLATWORM_MODULE_ABI_VERSION, sizeof(FlatwormModuleDefinition), "lifecycle", "1.0", "",
            initialize, shutdown, nullptr, 0, nullptr, 0};
}

TEST_F(FlatwormModuleSessions, CapabilityQueriesAreInertAndCloseReleasesRuntimeInstances) {
    initialized = shut_down = 0;
    fail_initialize = false;
    browser.modules().register_static(lifecycle_module());
    EXPECT_TRUE(browser.capabilities().has("javascript-modules"));
    EXPECT_TRUE(browser.capabilities().has("javascript-modules"));
    EXPECT_EQ(initialized, 0);
    auto session = browser.create_session();
    EXPECT_EQ(initialized, 1);
    EXPECT_TRUE(session->capabilities().has("javascript-modules"));
    EXPECT_EQ(initialized, 1);
    {
        auto standalone = browser.create_javascript_runtime();
        EXPECT_EQ(standalone->modules().size(), 1u);
        EXPECT_EQ(initialized, 2);
    }
    EXPECT_EQ(shut_down, 1);
    session->close();
    EXPECT_EQ(shut_down, 2);
    session->close();
    session.reset();
    EXPECT_EQ(shut_down, 2);
    EXPECT_EQ(browser.live_session_count(), 0u);
}

TEST_F(FlatwormModuleSessions, FailedSessionInitializationReleasesPartialInstances) {
    initialized = shut_down = 0;
    fail_initialize = true;
    browser.modules().load_native(TEST_MODULE_PATH);
    browser.modules().register_static(lifecycle_module());
    EXPECT_THROW(browser.create_session(), prowsetk::Error);
    EXPECT_EQ(browser.live_session_count(), 0u);
    EXPECT_EQ(initialized, 1);
    EXPECT_EQ(shut_down, 1);
    fail_initialize = false;
    auto session = browser.create_session();
    EXPECT_EQ(session->evaluate_js("Flatworm.module('fixture').next()"), "1");
    session.reset();
    EXPECT_EQ(shut_down, 2);
}

TEST_F(FlatwormModuleSessions, CloseFromScriptCallbacksDefersTeardownUntilEvaluationReturns) {
    initialized = shut_down = 0;
    fail_initialize = false;
    browser.modules().load_native(TEST_MODULE_PATH);
    browser.modules().register_static(lifecycle_module());
    auto session = browser.create_session();
    const auto subscription = browser.events().subscribe(prowsetk::EventType::Console,
        [&](const prowsetk::Event&) {
            session->close();
            EXPECT_EQ(shut_down, 0);
            EXPECT_THROW(session->evaluate_js("1"), prowsetk::Error);
        });
    EXPECT_EQ(session->evaluate_js("console.log('close'); Flatworm.module('fixture').next()"), "1");
    EXPECT_EQ(shut_down, 1);
    EXPECT_THROW(session->evaluate_js("1"), prowsetk::Error);
    browser.events().unsubscribe(subscription);
    session.reset();
    EXPECT_EQ(shut_down, 1);

    shut_down = 0;
    session = browser.create_session();
    const auto next_subscription = browser.events().subscribe(prowsetk::EventType::Console,
        [&](const prowsetk::Event&) { session->close(); EXPECT_EQ(shut_down, 0); });
    session->load_html("<p>before</p><script>document.querySelector('p').textContent = 'first';"
        "console.log('close');</script><script>document.querySelector('p').textContent = 'second';</script>");
    EXPECT_EQ(session->document()->query_selector("p")->text(), "first");
    EXPECT_EQ(shut_down, 1);
    browser.events().unsubscribe(next_subscription);
}

TEST_F(FlatwormModuleSessions, LuaUsesNativeBindingsThroughItsManagedPageEvaluation) {
    if (!prowsetk::LuaRuntime::available()) GTEST_SKIP();
    browser.modules().load_native(TEST_MODULE_PATH);
    auto session = browser.create_session();
    prowsetk::LuaRuntime lua;
    lua.bind_browser(&browser);
    lua.bind_session(session);
    const auto result = lua.run(R"lua(
        session:load_html('<p id="value"></p>', 'https://example.test/')
        assert(session:evaluate_js("Flatworm.module('fixture').echo('Lua-driven')") == 'Lua-driven')
        session:evaluate_js("document.querySelector('p').textContent = Flatworm.module('fixture').next()")
        assert(session:document():query_selector('p'):text() == '1')
    )lua");
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_TRUE(network->requests().empty());
}

TEST(FlatwormModuleDisabledSessions, DoNotInitializeNativeBindingsWhenJavaScriptIsDisabled) {
    initialized = shut_down = 0;
    fail_initialize = true;
    prowsetk::BrowserConfig config;
    config.javascript = false;
    prowsetk::Browser browser(config);
    browser.modules().register_static(lifecycle_module());
    auto session = browser.create_session();
    session->load_html("<p>offline</p>");
    EXPECT_EQ(session->document()->text(), "offline");
    EXPECT_THROW(session->evaluate_js("Flatworm.module('lifecycle')"), prowsetk::Error);
    session->close();
    EXPECT_EQ(initialized, 0);
    EXPECT_EQ(shut_down, 0);
}

}  // namespace
