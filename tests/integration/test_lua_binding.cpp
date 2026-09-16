#include <gtest/gtest.h>

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
