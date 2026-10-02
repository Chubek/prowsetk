#include <gtest/gtest.h>

#include "prowsetk/browser.hpp"
#include "prowsetk/lua_runtime.hpp"

TEST(LuaRuntimeLifetime, CollectedSessionInvalidatesHandlersAndRuntimeUnsubscribes) {
    if (!prowsetk::LuaRuntime::available()) GTEST_SKIP() << "Lua unavailable";
    prowsetk::BrowserConfig config;
    config.javascript = false;
    prowsetk::Browser browser(config);
    {
        prowsetk::LuaRuntime lua;
        lua.bind_browser(&browser);
        // Mirrors manual/09-lua-control-api.md "Managed handle lifetime".
        auto result = lua.run(R"LUA(
            prowse = require('lprowse')
            events = 0
            driver = prowse.browser.new():create_session()
            driver:on('document_created', function() events = events + 1 end)
            driver:load_html('<p>first</p>')
            assert(events == 1)
        )LUA");
        ASSERT_TRUE(result.ok) << result.error;
        result = lua.run(R"LUA(
            do
                local watched = prowse.browser.new():create_session()
                watched:on('document_created', function() events = events + 100 end)
                watched:load_html('<p>second</p>')
                assert(events == 101)
            end
            collectgarbage('collect')
            collectgarbage('collect')
            driver:load_html('<p>third</p>')
            assert(events == 102)  -- driver fires; the collected handle stays silent
        )LUA");
        ASSERT_TRUE(result.ok) << result.error;
        // Both handlers are still registered: the collected handle's callback is
        // inert (guarded by its lifetime flag) but is unregistered only at
        // runtime teardown.
        EXPECT_EQ(browser.events().handler_count(prowsetk::EventType::DocumentCreated), 2u);
    }
    EXPECT_EQ(browser.events().handler_count(prowsetk::EventType::DocumentCreated), 0u);
}

TEST(LuaRuntimeLifetime, OwnedBrowserOutlivesItsSessions) {
    if (!prowsetk::LuaRuntime::available()) GTEST_SKIP() << "Lua unavailable";
    // Sessions hold a raw Browser* and Session::~Session dereferences it, so a
    // Lua-owned Browser must not be deleted while a session still exists.
    // `prowse.browser.new():create_session()` drops the browser handle
    // immediately; every use below would dangle without deferred release.
    prowsetk::LuaRuntime lua;
    const auto result = lua.run(R"LUA(
        local prowse = require('lprowse')

        -- The browser handle is dropped immediately by this expression while
        -- the session stays reachable: every later use reaches Browser*.
        local session = prowse.browser.new():create_session()
        session:load_html('<title>Retained</title><button id="go">Go</button>',
                          'https://example.test/')
        session = nil
        collectgarbage('collect')
        collectgarbage('collect')

        -- A document obtained before both handles were dropped keeps its own
        -- Session alive, so it must stay usable, including the
        -- session-mediated synthetic interaction that dereferences Browser*.
        local owner = prowse.browser.new():create_session()
        owner:load_html('<title>Doc holder</title><button id="go">Go</button>')
        local document = owner:document()
        owner = nil
        collectgarbage('collect')
        collectgarbage('collect')

        assert(document:title() == 'Doc holder')
        local button = document:query_selector('#go')
        assert(button ~= nil and button:click() == true)
        assert(document:title() == 'Doc holder')
    )LUA", "owned_browser_lifetime");
    EXPECT_TRUE(result.ok) << result.error;
}

TEST(LuaRuntimeLifetime, CollectedExtractorInvalidatesInstalledCallback) {
    if (!prowsetk::LuaRuntime::available()) GTEST_SKIP() << "Lua unavailable";
    prowsetk::BrowserConfig config;
    config.javascript = false;
    prowsetk::Browser browser(config);
    {
        prowsetk::LuaRuntime lua;
        lua.bind_browser(&browser);
        const auto result = lua.run(R"LUA(
            local browser = require('lprowse').browser.new()
            local session = browser:create_session()
            local extractor = require('lprowsext').extractor.new()
            local calls = 0
            extractor:on_document(function() calls = calls + 1 end)
            browser:install_extension(extractor)
            session:load_html('<p>First</p>')
            assert(calls == 1)
            extractor = nil
            collectgarbage('collect')
            collectgarbage('collect')
            session:load_html('<p>Second</p>')
            assert(calls == 1)
        )LUA");
        EXPECT_TRUE(result.ok) << result.error;
        EXPECT_EQ(browser.events().handler_count(prowsetk::EventType::DocumentCreated), 1u);
    }
    EXPECT_EQ(browser.events().handler_count(prowsetk::EventType::DocumentCreated), 0u);
}
