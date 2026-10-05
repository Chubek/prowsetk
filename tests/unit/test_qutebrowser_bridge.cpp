#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "prowsetk/lua_runtime.hpp"

namespace {
using prowsetk::LuaRuntime;

bool setup(LuaRuntime& lua) {
    return lua.run("package.loaded.lquteipc = assert(package.loadlib('" QUTE_IPC_PATH
                   "', 'luaopen_lquteipc'))(); "
                   "qute = dofile('" PROWSETK_SOURCE_DIR
                   "/tools/qutebrowser-bridge/lua/qutebrowser_bridge.lua')").ok;
}

TEST(QutebrowserBridge, JsonStrictnessUnicodeAndArrayShape) {
    LuaRuntime lua;
    ASSERT_TRUE(setup(lua)) << lua.last_error();
    const auto result = lua.run(R"LUA(
        local j = qute.json
        local value = j.decode([[{"array":[],"unicode":"\uD83D\uDE00","flag":false}]])
        assert(value.unicode == utf8.char(0x1f600))
        assert(j.encode(value):find('"array":[]',1,true))
        assert(not pcall(j.decode, '{"x":1,"x":2}'))
        assert(not pcall(j.decode, '{"x":01}'))
        assert(not pcall(j.decode, '{"x":1e999}'))
        assert(not pcall(j.decode, [[{"x":"\uD800"}]]))
        assert(not pcall(j.decode, string.rep('[',34)..string.rep(']',34)))
        local text = '"; _G.executed = true; --'
        assert(j.decode(j.encode({value=text})).value == text)
        assert(executed == nil)
    )LUA");
    EXPECT_TRUE(result.ok) << result.error;
}

TEST(QutebrowserBridge, SnapshotScriptsAreInertAndPluginExportsAreRedacted) {
    LuaRuntime lua;
    ASSERT_TRUE(setup(lua)) << lua.last_error();
    const auto result = lua.run(R"LUA(
        local active, browser = qute.load_snapshot{
            url='https://example.test/', html=[[
                <h1>Assistant</h1><script>globalThis.executed=true;fetch('/api/items?token=private_marker')</script>
                <form action='/api/items' method='post'><input name='token' value='private_marker'>
                <input name='customer' value='private_form_marker'>
                <input name='amount' type='number' required value='3'></form>
            ]]
        }
        local before = qute.scrape(active)
        assert(#before.endpoints >= 2)
        local enriched = qute.enrich(active, before.endpoints)
        assert(enriched.probe_count == 0)
        assert(enriched.openapi_yaml:find('x-prowsetk-schema:',1,true))
        assert(not enriched.openapi_yaml:find('private_marker',1,true))
        assert(not enriched.postman_json:find('private_marker',1,true))
        assert(not enriched.openapi_yaml:find('private_form_marker',1,true))
        assert(not enriched.postman_json:find('private_form_marker',1,true))
        assert(active:document():query_selector('input[name="customer"]'):attribute('value') == 'private_form_marker')
        assert(require('lpdql').query(active, 'select text from <h1>'):find('Assistant',1,true))
        assert(not pcall(function() active:evaluate_js('globalThis.executed') end))
        active:close()
    )LUA");
    EXPECT_TRUE(result.ok) << result.error;
}

TEST(QutebrowserBridge, NativeFilePermissionsAtomicFailureAndSafeErrors) {
    LuaRuntime lua;
    ASSERT_TRUE(setup(lua)) << lua.last_error();
    const auto root = std::filesystem::path(TEST_BINARY_DIR) / "qute-native";
    std::filesystem::create_directories(root);
    const auto file = root / "output.json";
    ASSERT_TRUE(lua.run("qute.write('" + file.string() + "', '{\"ok\":true}'); "
                        "assert(package.loaded.lquteipc.read_private('" + file.string() + "'))").ok) << lua.last_error();
    EXPECT_EQ(std::filesystem::status(file).permissions() & std::filesystem::perms::all,
              std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
    std::filesystem::permissions(file, std::filesystem::perms::group_read, std::filesystem::perm_options::add);
    EXPECT_TRUE(lua.run("assert(package.loaded.lquteipc.read_private('" + file.string() + "') == nil)").ok);
    const auto link = root / "link.json";
    std::filesystem::remove(link);
    std::filesystem::create_symlink(file, link);
    EXPECT_TRUE(lua.run("assert(package.loaded.lquteipc.read_private('" + link.string() + "') == nil)").ok);
    EXPECT_TRUE(lua.run(R"LUA(
        local value, err = package.loaded.lquteipc.exchange('/missing/private_marker', '{}', 10)
        assert(value == nil and err == 'qutebrowser bridge operation failed')
        assert(not err:find('private_marker',1,true))
        assert(package.loaded.lquteipc.exchange('/missing', '{}', 32001) == nil)
    )LUA").ok) << lua.last_error();
    std::filesystem::remove_all(root);
}
}  // namespace
