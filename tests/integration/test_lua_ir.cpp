#include <gtest/gtest.h>

#include "prowsetk/lua_runtime.hpp"

using prowsetk::LuaRuntime;

TEST(Lprowseir, RequiresModuleAndAliases) {
    if (!LuaRuntime::available()) {
        GTEST_SKIP() << "ProwseTk was built without Lua support";
    }
    LuaRuntime lua;
    const auto result = lua.run(R"LUA(
        local ir = require("lprowseir")
        assert(ir ~= nil)
        assert(ir.dom ~= nil)
        assert(ir.xas ~= nil)
        assert(ir.xax ~= nil)
        assert(require("lprowseir.xas") == ir.xas)
        assert(require("lprowseir.xax") == ir.xax)
        assert(require("lprowseir.dom") == ir.dom)
    )LUA", "lprowseir_require");
    EXPECT_TRUE(result.ok) << lua.last_error();
}

TEST(Lprowseir, EmitsAllBuiltInFormats) {
    if (!LuaRuntime::available()) {
        GTEST_SKIP() << "ProwseTk was built without Lua support";
    }
    LuaRuntime lua;
    const auto result = lua.run(R"LUA(
        local prowse = require("lprowse")
        local ir = require("lprowseir")
        local browser = prowse.browser.new()
        local session = browser:create_session()
        session:load_html([[<html><body><a href='/go'>Go</a></body></html>]],
                          "https://example.com/")

        local xas = ir.emit_xas(session)
        local dom = ir.emit_dom(session)
        local vtd = ir.emit_vtd(session)
        local iml = ir.emit_iml(session)
        assert(#xas > 0)
        assert(#dom > 0)
        assert(type(vtd) == "string" and #vtd > 5)
        assert(type(iml) == "string" and string.find(iml, "%(document") ~= nil)
    )LUA", "lprowseir_emit");
    EXPECT_TRUE(result.ok) << lua.last_error();
}

TEST(Lprowseir, DomWalkUsesXPath) {
    if (!LuaRuntime::available()) {
        GTEST_SKIP() << "ProwseTk was built without Lua support";
    }
    LuaRuntime lua;
    const auto result = lua.run(R"LUA(
        local prowse = require("lprowse")
        local ir = require("lprowseir")
        local browser = prowse.browser.new()
        local session = browser:create_session()
        session:load_html([[<html><body><ul><li>a</li><li>b</li></ul></body></html>]])

        local count = 0
        local walked = ir.dom.walk(session, "//li", function(node)
            count = count + 1
            assert(node:tag_name() == "li")
        end)
        assert(walked == 2 and count == 2)
    )LUA", "lprowseir_walk");
    EXPECT_TRUE(result.ok) << lua.last_error();
}

TEST(Lprowseir, XasAddListenerFiltersWithXPath) {
    if (!LuaRuntime::available()) {
        GTEST_SKIP() << "ProwseTk was built without Lua support";
    }
    LuaRuntime lua;
    const auto result = lua.run(R"LUA(
        local prowse = require("lprowse")
        local ir = require("lprowseir")
        local browser = prowse.browser.new()
        local session = browser:create_session()
        session:load_html([[<html><body><table><tr><td>x</td><td>y</td></tr></table></body></html>]])

        local seen = 0
        local count = ir.xas:AddListener(session, "//td", function(event)
            if event.kind == "text" then
                seen = seen + 1
            end
        end)
        assert(count > 0)
        assert(seen >= 2)
    )LUA", "lprowseir_listener");
    EXPECT_TRUE(result.ok) << lua.last_error();
}

TEST(Lprowseir, XaxAliasSupportsAddListener) {
    if (!LuaRuntime::available()) {
        GTEST_SKIP() << "ProwseTk was built without Lua support";
    }
    LuaRuntime lua;
    const auto result = lua.run(R"LUA(
        local prowse = require("lprowse")
        local ir = require("lprowseir")
        local browser = prowse.browser.new()
        local session = browser:create_session()
        session:load_html([[<html><body><td>z</td></body></html>]])
        local called = 0
        ir.xax:AddListener(session, "//td", function(_)
            called = called + 1
        end)
        assert(called > 0)
    )LUA", "lprowseir_xax_alias");
    EXPECT_TRUE(result.ok) << lua.last_error();
}

TEST(Lprowseir, RegistryPipelineEmitsByName) {
    if (!LuaRuntime::available()) {
        GTEST_SKIP() << "ProwseTk was built without Lua support";
    }
    LuaRuntime lua;
    const auto result = lua.run(R"LUA(
        local prowse = require("lprowse")
        local ir = require("lprowseir")
        local browser = prowse.browser.new()
        local session = browser:create_session()
        session:load_html([[<html><body><a href='/go'>Go</a></body></html>]],
                          "https://example.com/")

        local names = ir.emitters()
        local has_iml = false
        local has_vtd = false
        for _, name in ipairs(names) do
            if name == "iml" then has_iml = true end
            if name == "vtd" then has_vtd = true end
        end
        assert(has_iml and has_vtd)

        local iml = ir.emit(session, "iml")
        assert(type(iml) == "string" and string.find(iml, "%(document") ~= nil)
        local vtd = ir.emit(session, "vtd")
        assert(type(vtd) == "string" and #vtd > 5)

        local ok = pcall(function() ir.emit(session, "no-such-ir") end)
        assert(ok == false)
    )LUA", "lprowseir_registry");
    EXPECT_TRUE(result.ok) << lua.last_error();
}

TEST(Lprowseir, WalkRejectsInvalidXPath) {
    if (!LuaRuntime::available()) {
        GTEST_SKIP() << "ProwseTk was built without Lua support";
    }
    LuaRuntime lua;
    const auto result = lua.run(R"LUA(
        local prowse = require("lprowse")
        local ir = require("lprowseir")
        local browser = prowse.browser.new()
        local session = browser:create_session()
        session:load_html([[<html><body><li>a</li></body></html>]])

        local ok, err = pcall(function()
            ir.dom.walk(session, "//*[", function() end)
        end)
        assert(ok == false)
        assert(string.find(err, "invalid XPath") ~= nil)
    )LUA", "lprowseir_walk_legality");
    EXPECT_TRUE(result.ok) << lua.last_error();
}

TEST(Lprowseir, ListenerRejectsInvalidXPath) {
    if (!LuaRuntime::available()) {
        GTEST_SKIP() << "ProwseTk was built without Lua support";
    }
    LuaRuntime lua;
    const auto result = lua.run(R"LUA(
        local prowse = require("lprowse")
        local ir = require("lprowseir")
        local browser = prowse.browser.new()
        local session = browser:create_session()
        session:load_html([[<html><body><td>z</td></body></html>]])

        local ok, err = pcall(function()
            ir.xas:AddListener(session, "//td[", function() end)
        end)
        assert(ok == false)
        assert(string.find(err, "invalid XPath") ~= nil)
    )LUA", "lprowseir_listener_legality");
    EXPECT_TRUE(result.ok) << lua.last_error();
}

TEST(Lprowseir, WalkPropagatesCallbackErrors) {
    if (!LuaRuntime::available()) {
        GTEST_SKIP() << "ProwseTk was built without Lua support";
    }
    LuaRuntime lua;
    const auto result = lua.run(R"LUA(
        local prowse = require("lprowse")
        local ir = require("lprowseir")
        local browser = prowse.browser.new()
        local session = browser:create_session()
        session:load_html([[<html><body><ul><li>a</li><li>b</li></ul></body></html>]])

        local ok, err = pcall(function()
            ir.dom.walk(session, "//li", function(_)
                error("walk-boom")
            end)
        end)
        assert(ok == false)
        assert(string.find(err, "walk%-boom") ~= nil)
    )LUA", "lprowseir_walk_errors");
    EXPECT_TRUE(result.ok) << lua.last_error();
}

TEST(Lprowseir, ListenerPropagatesCallbackErrors) {
    if (!LuaRuntime::available()) {
        GTEST_SKIP() << "ProwseTk was built without Lua support";
    }
    LuaRuntime lua;
    const auto result = lua.run(R"LUA(
        local prowse = require("lprowse")
        local ir = require("lprowseir")
        local browser = prowse.browser.new()
        local session = browser:create_session()
        session:load_html([[<html><body><td>z</td></body></html>]])

        local ok, err = pcall(function()
            ir.xas:AddListener(session, "//td", function(_)
                error("listener-boom")
            end)
        end)
        assert(ok == false)
        assert(string.find(err, "listener%-boom") ~= nil)
    )LUA", "lprowseir_listener_errors");
    EXPECT_TRUE(result.ok) << lua.last_error();
}
