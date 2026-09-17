#include <gtest/gtest.h>

#include <fstream>

#include "prowsetk/browser.hpp"
#include "prowsetk/lua_runtime.hpp"

using prowsetk::Browser;
using prowsetk::LuaRuntime;

TEST(Lprowsext, ProcessesDocumentAndCollectsResults) {
    if (!LuaRuntime::available()) {
        GTEST_SKIP() << "ProwseTk was built without Lua support";
    }
    LuaRuntime lua;
    const auto result = lua.run(R"LUA(
        local ext = require("lprowsext")
        local browser = require("lprowse").browser.new()
        local session = browser:create_session()
        session:load_html("<title>Processor regression</title>")
        local document = session:document()
        ext.register_document_processor("title", function(d)
            assert(d == document)
            return d:title()
        end)
        local results = ext.process_document(document)
        assert(results.title == "Processor regression")
    )LUA", "processor_regression");
    EXPECT_TRUE(result.ok) << lua.last_error();
}

TEST(Lprowsext, EmptyProcessorRegistryReturnsTable) {
    if (!LuaRuntime::available()) {
        GTEST_SKIP() << "ProwseTk was built without Lua support";
    }
    LuaRuntime lua;
    EXPECT_TRUE(lua.run(R"LUA(
        local ext = require("lprowsext")
        local browser = require("lprowse").browser.new()
        local session = browser:create_session()
        session:load_html("<p>Empty registry</p>")
        local result = ext.process_document(session:document())
        assert(type(result) == "table" and next(result) == nil)
    )LUA").ok) << lua.last_error();
}

TEST(Lprowsext, ProcessorFailuresDoNotDiscardOtherResults) {
    if (!LuaRuntime::available()) {
        GTEST_SKIP() << "ProwseTk was built without Lua support";
    }
    LuaRuntime lua;
    EXPECT_TRUE(lua.run(R"LUA(
        local ext = require("lprowsext")
        local browser = require("lprowse").browser.new()
        local session = browser:create_session()
        session:load_html("<title>Results</title><p>one</p><p>two</p>")
        ext.register_document_processor("failure", function() error("fixture failure") end)
        ext.register_document_processor("nothing", function() return nil end)
        ext.register_document_processor("false_value", function() return false end)
        ext.register_document_processor("count", function(d) return #d:query_selector_all("p") end)
        ext.register_document_processor("structured", function(d) return {title = d:title()} end)
        for i = 1, 20 do
            local result = ext.process_document(session:document())
            assert(result.failure == nil and result.nothing == nil)
            assert(result.false_value == false and result.count == 2)
            assert(result.structured.title == "Results")
        end
    )LUA").ok) << lua.last_error();
}

TEST(Lprowsext, RequiresModule) {
    if (!LuaRuntime::available()) {
        GTEST_SKIP() << "ProwseTk was built without Lua support";
    }
    LuaRuntime lua;
    const auto result =
        lua.run(R"LUA(
            local ext = require("lprowsext")
            assert(ext ~= nil, "lprowsext missing")
            assert(ext.dom ~= nil, "lprowsext.dom missing")
            assert(ext.endpoints ~= nil, "lprowsext.endpoints missing")
            assert(ext.extractor ~= nil, "lprowsext.extractor missing")
            assert(ext.wasm ~= nil, "lprowsext.wasm missing")
            assert(require("lprowsext.dom") == ext.dom,
                   "lprowsext.dom require mismatch")
            assert(require("lprowsext.endpoints") == ext.endpoints,
                   "lprowsext.endpoints require mismatch")
            assert(require("lprowsext.extractor") == ext.extractor,
                   "lprowsext.extractor require mismatch")
            assert(require("lprowsext.wasm") == ext.wasm,
                   "lprowsext.wasm require mismatch")
        )LUA", "lprowsext_requires");
    EXPECT_TRUE(result.ok) << lua.last_error();
}

TEST(Lprowsext, XPathDomQueries) {
    if (!LuaRuntime::available()) {
        GTEST_SKIP() << "ProwseTk was built without Lua support";
    }
#ifdef PROWSETK_HAVE_PUGIXML
    LuaRuntime lua;
    const auto result = lua.run(R"LUA(
        local prowse = require("lprowse")
        local ext = require("lprowsext")
        local browser = prowse.browser.new()
        local session = browser:create_session()
        session:load_html([[
            <html><head><title>X</title></head><body>
              <ul><li class="a">1</li><li class="b">2</li></ul>
              <a href="/go">Go</a>
            </body></html>]], "https://example.com/")
        local document = session:document()

        local items = document:xpath("//li")
        assert(#items == 2, "expected 2 li, got " .. #items)

        local href = document:xpath_strings("//a/@href")
        assert(#href == 1 and href[1] == "/go",
               "expected /go href, got " .. tostring(href[1]))

        local count = ext.dom.xpath(document, "count(//li)")
        assert(count == 2, "expected count 2, got " .. tostring(count))

        local b = document:query_selector("li.b")
        local nested = b:xpath(".//text()")
        assert(#nested >= 1, "expected text nodes under li.b")
    )LUA", "lprowsext_xpath");
    EXPECT_TRUE(result.ok) << lua.last_error();
#else
    GTEST_SKIP() << "pugixml not available in this build";
#endif
}

TEST(Lprowsext, ExtractorRegistersAndRuns) {
    if (!LuaRuntime::available()) {
        GTEST_SKIP() << "ProwseTk was built without Lua support";
    }
    LuaRuntime lua;
    const auto result = lua.run(R"LUA(
        local prowse = require("lprowse")
        local ext = require("lprowsext")
        local browser = prowse.browser.new()
        local session = browser:create_session()
        session:load_html([[<html><head><title>Extract</title></head></html>]])

        local extractor = ext.extractor.new()
        local seen = nil
        extractor:on_document(function(document)
            seen = document:title()
            return { title = seen }
        end)

        local value = extractor:run(session:document())
        assert(seen == "Extract", "extractor callback did not run")
        assert(value.title == "Extract", "extractor result missing")

        assert(ext.register_document_processor("titles", function(d)
            return d:title()
        end))
        assert(ext.get_document_processor("titles") ~= nil)
    )LUA", "lprowsext_extractor");
    EXPECT_TRUE(result.ok) << lua.last_error();
}

TEST(Lprowsext, EndpointExtractionFromLua) {
    if (!LuaRuntime::available()) {
        GTEST_SKIP() << "ProwseTk was built without Lua support";
    }
    LuaRuntime lua;
    const auto result = lua.run(R"LUA(
        local prowse = require("lprowse")
        local ext = require("lprowsext")
        local browser = prowse.browser.new()
        local session = browser:create_session()
        session:load_html([[
            <html><head><title>API</title></head><body>
              <a href="/api/users">Users</a>
              <form action="/api/login" method="POST">
                <input name="username">
              </form>
              <script>fetch('/api/items?token=sec&page=1');</script>
            </body></html>]], "https://example.com/")

        local result = ext.endpoints.extract(session:document(), {
            follow_links = true,
            inspect_scripts = true,
            redact_secrets = true
        })
        assert(result ~= nil, "endpoint result missing")
        assert(result:endpoint_count() >= 2,
               "expected >=2 endpoints, got " .. result:endpoint_count())

        local yaml = result:openapi_yaml()
        assert(string.find(yaml, "openapi:") ~= nil, "missing openapi header")
        assert(string.find(yaml, "/api/login") ~= nil, "missing login path")
        assert(string.find(yaml, "supersecret") == nil or
               string.find(yaml, "supersecret") == string.find(yaml, "REDACTED"),
               "secret not redacted")

        local endpoints = result:endpoints()
        assert(#endpoints >= 1, "endpoints() empty")

        local warnings = result:warnings()
        assert(warnings ~= nil, "warnings missing")
    )LUA", "lprowsext_endpoints");
    EXPECT_TRUE(result.ok) << lua.last_error();
}

TEST(Lprowsext, EndpointResultWritesYaml) {
    if (!LuaRuntime::available()) {
        GTEST_SKIP() << "ProwseTk was built without Lua support";
    }
    const std::string output = std::string(TEST_BINARY_DIR) + "/openapi.yaml";
    LuaRuntime lua;
    const std::string script = R"LUA(
        local prowse = require("lprowse")
        local ext = require("lprowsext")
        local browser = prowse.browser.new()
        local session = browser:create_session()
        session:load_html([[<html><body>
          <a href="/api/health">Health</a>
        </body></html>]], "https://example.com/")

        local result = ext.endpoints.extract(session:document())
        local ok = result:write_openapi_yaml("@OUTPUT@")
        assert(ok, "write_openapi_yaml failed")
    )LUA";
    const std::string program =
        script.find("@OUTPUT@") != std::string::npos
            ? script.substr(0, script.find("@OUTPUT@")) + output +
                  script.substr(script.find("@OUTPUT@") + 8)
            : script;
    const auto result = lua.run(program, "lprowsext_write");
    EXPECT_TRUE(result.ok) << lua.last_error();

    std::ifstream stream(output);
    ASSERT_TRUE(stream.good());
    std::string content((std::istreambuf_iterator<char>(stream)),
                        std::istreambuf_iterator<char>());
    EXPECT_NE(content.find("openapi:"), std::string::npos);
}

TEST(Lprowsext, WasmSurfaceReflectsAvailability) {
    if (!LuaRuntime::available()) {
        GTEST_SKIP() << "ProwseTk was built without Lua support";
    }
    LuaRuntime lua;
    const auto result = lua.run(R"LUA(
        local ext = require("lprowsext")
        assert(ext.wasm.available() == false,
               "WASM should report unavailable in this build")
        local handle, err = ext.wasm.load("plugin.wasm", {})
        assert(handle == nil, "load should fail without a WASM runtime")
        assert(type(err) == "string" and #err > 0, "load should explain")
    )LUA", "lprowsext_wasm");
    EXPECT_TRUE(result.ok) << lua.last_error();
}
