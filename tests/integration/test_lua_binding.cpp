#include <gtest/gtest.h>

#include <memory>

#include "prowsetk/browser.hpp"
#include "prowsetk/lua_runtime.hpp"

using prowsetk::Browser;
using prowsetk::LuaRuntime;

TEST(LuaBinding, DrivesBrowserAndDocument) {
    if (!LuaRuntime::available()) {
        GTEST_SKIP() << "ProwseTk was built without Lua support";
    }
    LuaRuntime lua;
    const char* script = R"LUA(
        local prowsetk = require("lprowse")
        local browser = prowsetk.browser.new()
        local session = browser:create_session()
        session:load_html([[
            <html><head><title>Lua Page</title></head><body>
              <a class="nav" href="/one">One</a>
              <a href="/two">Two</a>
            </body></html>]], "https://example.com/")

        local document = session:document()
        assert(document ~= nil, "document missing")
        assert(document:title() == "Lua Page", "title mismatch: " .. document:title())

        local links = document:query_selector_all("a")
        assert(#links == 2, "expected 2 links, got " .. #links)
        assert(links[1]:attribute("href") == "/one")
        assert(links[1]:text() == "One")
        assert(links[2]:attribute("href") == "/two")

        local nav = document:query_selector("a.nav")
        assert(nav ~= nil, "class selector failed")
        assert(nav:tag_name() == "a")
        assert(nav:class_name() == "nav")

        local by_id = document:query_selector("#missing")
        assert(by_id == nil, "missing id should be nil")

        return "ok"
    )LUA";

    const auto result = lua.run(script, "lua_binding_test");
    EXPECT_TRUE(result.ok) << lua.last_error();
}

TEST(LuaBinding, BrowserNewUsesBoundBrowserDefaults) {
    if (!LuaRuntime::available()) {
        GTEST_SKIP() << "ProwseTk was built without Lua support";
    }

    prowsetk::BrowserConfig config;
    config.user_agent = "BoundAgent/7.0";
    Browser browser(config);

    auto network = std::make_unique<prowsetk::MemoryNetworkClient>();
    auto* network_ptr = network.get();
    prowsetk::HttpResponse response;
    response.status = 204;
    response.final_url = "https://example.test/";
    network->set_response("https://example.test/", response);
    browser.set_network_client(std::move(network));

    LuaRuntime lua;
    lua.bind_browser(&browser);
    const auto result = lua.run(R"LUA(
        local prowse = require("lprowse")
        local browser = prowse.browser.new()
        local session = browser:create_session()
        session:request("GET", "https://example.test/")
    )LUA", "lua_bound_browser_user_agent");
    ASSERT_TRUE(result.ok) << lua.last_error();
    ASSERT_EQ(network_ptr->requests().size(), 1u);

    bool found = false;
    for (const auto& [name, value] : network_ptr->requests()[0].headers) {
        if (name == "User-Agent") {
            found = true;
            EXPECT_EQ(value, "BoundAgent/7.0");
        }
    }
    EXPECT_TRUE(found);
}

TEST(LuaBinding, InspectsAndMutatesDocuments) {
    if (!LuaRuntime::available()) {
        GTEST_SKIP() << "ProwseTk was built without Lua support";
    }
    LuaRuntime lua;
    const auto result = lua.run(R"LUA(
        local prowse = require("lprowse")
        local browser = prowse.browser.new()
        local session = browser:create_session()
        session:load_html([[
            <html><head><script src="/app.js"></script></head><body>
              <form action="/login"><input name="user"></form>
              <ul><li id="first">One</li><li id="second">Two</li></ul>
            </body></html>]], "https://example.com/base/")

        session:set_header("X-Trace", "lua")
        assert(session:headers()["X-Trace"] == "lua")
        session:clear_headers()
        assert(next(session:headers()) == nil)

        local document = session:document()
        assert(document:root() ~= nil, "document root missing")
        assert(#document:get_elements_by_tag_name("li") == 2)
        assert(#document:forms() == 1)
        assert(#document:scripts() == 1)
        assert(#document:resource_urls() >= 1)

        local first = document:get_element_by_id("first")
        local second = document:get_element_by_id("second")
        assert(first:next_sibling():id() == "second")
        assert(second:previous_sibling():id() == "first")
        assert(first:parent():tag_name() == "ul")
        assert(first:parent():first_child():id() == "first")

        local body = document:query_selector("body")
        local notice = document:create_element("p")
        assert(notice:set_attribute("class", "notice"))
        assert(notice:attributes().class == "notice")
        assert(notice:remove_attribute("class"))
        assert(not notice:has_attribute("class"))
        assert(notice:set_text("Added from Lua"))
        assert(body:append_child(notice))
        assert(document:query_selector("p"):text() == "Added from Lua")
        assert(body:remove_child(notice))
        assert(document:query_selector("p") == nil)
    )LUA", "lua_document_mutation");
    EXPECT_TRUE(result.ok) << lua.last_error();
}

TEST(LuaBinding, ReportsScriptErrors) {
    if (!LuaRuntime::available()) {
        GTEST_SKIP() << "ProwseTk was built without Lua support";
    }
    LuaRuntime lua;
    const auto result = lua.run("error('boom')", "failing");
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("boom"), std::string::npos);
}

TEST(LuaBinding, CallableFunctions) {
    if (!LuaRuntime::available()) {
        GTEST_SKIP() << "ProwseTk was built without Lua support";
    }
    LuaRuntime lua;
    ASSERT_TRUE(lua.run("function main() return 42 end").ok);
    EXPECT_TRUE(lua.call("main").ok);
    EXPECT_FALSE(lua.call("does_not_exist").ok);
}

TEST(LuaBinding, CallFunctionWithTypedArguments) {
    if (!LuaRuntime::available()) {
        GTEST_SKIP() << "ProwseTk was built without Lua support";
    }
    LuaRuntime lua;
    ASSERT_TRUE(
        lua.run("function describe(args) "
                "return tostring(args.url) .. '|' .. tostring(args.depth) .. "
                "'|' .. tostring(args.flag) end")
            .ok);
    const std::vector<prowsetk::LuaArgument> args = {
        {"url", "string", "https://example.com/"},
        {"depth", "integer", "3"},
        {"flag", "boolean", "true"},
    };
    std::string result;
    ASSERT_TRUE(lua.call_function("describe", args, &result).ok)
        << lua.last_error();
    EXPECT_EQ(result, "https://example.com/|3|true");
}

TEST(LuaBinding, CallFunctionCapturesNumericReturn) {
    if (!LuaRuntime::available()) {
        GTEST_SKIP() << "ProwseTk was built without Lua support";
    }
    LuaRuntime lua;
    ASSERT_TRUE(lua.run("function finish(args) return tonumber(args.code) end")
                    .ok);
    std::string result;
    ASSERT_TRUE(lua.call_function("finish", {{"code", "integer", "7"}},
                                  &result)
                    .ok)
        << lua.last_error();
    EXPECT_EQ(result, "7");
}

TEST(LuaBinding, CallFunctionReportsErrors) {
    if (!LuaRuntime::available()) {
        GTEST_SKIP() << "ProwseTk was built without Lua support";
    }
    LuaRuntime lua;
    ASSERT_TRUE(lua.run("function boom() error('kaboom') end").ok);
    std::string result;
    const auto call = lua.call_function("boom", {}, &result);
    EXPECT_FALSE(call.ok);
    EXPECT_NE(lua.last_error().find("kaboom"), std::string::npos);

    const auto missing = lua.call_function("nope", {}, &result);
    EXPECT_FALSE(missing.ok);
    EXPECT_NE(lua.last_error().find("nope"), std::string::npos);
}

TEST(LuaBinding, SyntheticClickAndTypeDispatchCascades) {
    if (!LuaRuntime::available()) {
        GTEST_SKIP() << "ProwseTk was built without Lua support";
    }
    LuaRuntime lua;
    const char* script = R"LUA(
        local prowse = require("lprowse")
        local browser = prowse.browser.new()
        local session = browser:create_session()
        session:load_html([[
            <html><body>
              <button id="go" type="button">Go</button>
              <button id="ghost" type="button" style="display: none">Ghost</button>
              <input id="q" type="text" value="">
              <script>
                window.__cascade = [];
                var go = document.getElementById("go");
                ["pointerover","pointerenter","pointerdown","mousedown",
                 "focus","pointerup","mouseup","click"].forEach(function (t) {
                  go.addEventListener(t, function () { window.__cascade.push(t); });
                });
                window.__inputs = 0;
                document.getElementById("q").addEventListener("input", function () {
                  window.__inputs += 1;
                });
              </script>
            </body></html>]], "https://example.com/")

        local document = session:document()
        local go = document:query_selector("#go")
        assert(go ~= nil, "button missing")
        assert(go:click() == true, "elem:click() should dispatch")
        local order = session:evaluate_js("window.__cascade.join('|')")
        assert(order == "pointerover|pointerenter|pointerdown|mousedown|focus|"
               .. "pointerup|mouseup|click",
               "cascade order wrong: " .. order)

        -- Session-mediated entry points reach the same cascade.
        session:evaluate_js("window.__cascade = []")
        assert(session:click_element(go) == true, "session:click_element failed")
        assert(session:evaluate_js("window.__cascade.length") == "8",
               "expected 8 events")

        -- Typing updates the value through the native setter path.
        local input = document:query_selector("#q")
        assert(input ~= nil, "input missing")
        assert(input:type("ab") == true, "elem:type() should dispatch")
        assert(input:value() == "ab", "value mismatch: " .. input:value())
        assert(session:evaluate_js("window.__inputs") == "2",
               "expected one input event per char")
        assert(session:type_element(input, "") == true, "empty type should commit")

        -- Non-interactable elements fail fast instead of dropping events.
        local ghost = document:query_selector("#ghost")
        assert(ghost ~= nil, "ghost missing")
        assert(ghost:click() == false, "hidden click should fail fast")
        assert(ghost:type("x") == false, "hidden type should fail fast")
        assert(session:click_element(ghost) == false,
               "session hidden click should fail")

        return "ok"
    )LUA";

    const auto result = lua.run(script, "lua_synthetic_interactions");
    EXPECT_TRUE(result.ok) << lua.last_error();
}
