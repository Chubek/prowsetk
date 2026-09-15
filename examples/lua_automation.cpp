// Demonstrates driving a session from Lua through the lprowse bindings. The
// example loads an in-memory document so it stays hermetic.

#include <iostream>

#include <prowsetk/browser.hpp>
#include <prowsetk/lua_runtime.hpp>

int main() {
    if (!prowsetk::LuaRuntime::available()) {
        std::cout << "This build has no Lua support.\n";
        return 0;
    }

    prowsetk::Browser browser;
    prowsetk::LuaRuntime lua;
    lua.bind_browser(&browser);

    const char* script = R"LUA(
        local prowsetk = require("lprowse")
        local browser = prowsetk.browser.new()
        local session = browser:create_session()
        session:load_html(
            "<html><head><title>Hello</title></head>" ..
            "<body><a href='/x'>X</a></body></html>",
            "https://example.com/")
        local document = session:document()
        print("title: " .. document:title())
        for _, link in ipairs(document:query_selector_all("a")) do
            print("link: " .. link:attribute("href"))
        end
    )LUA";

    const auto result = lua.run(script, "example");
    if (!result.ok) {
        std::cerr << "lua error: " << lua.last_error() << '\n';
        return 1;
    }
    return 0;
}
