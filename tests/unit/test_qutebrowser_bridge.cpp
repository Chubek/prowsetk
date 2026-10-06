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
        assert(not pcall(qute.load_snapshot, {url='https://example.test/', html=string.char(255)}))
        assert(not pcall(qute.load_snapshot, {url='https://example.test/', html='<p>'..string.char(0)..'</p>'}))
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

TEST(QutebrowserBridge, NoiseFilteringAndMultiPageSchemaSerialization) {
    LuaRuntime lua;
    ASSERT_TRUE(setup(lua)) << lua.last_error();
    const auto result = lua.run(R"LUA(
        local active, owner = qute.load_snapshot{url='https://example.test/', html=[[
            <a href='/api/items?limit=2'>Items</a><a href='/dashboard'>Plain page</a>
            <script>fetch('/js_errors', {method:'POST'}); fetch('/api/first?limit=2&token=private_marker');
            fetch('/telemetry', {method:'POST'});</script>
            <form action='/api/first' method='post'><input name='customer' value='private_form_marker'>
            <input name='count' type='number' required value='2'></form>
        ]]}
        local first = qute.scrape(active)
        local paths = {}
        for _, ep in ipairs(first.endpoints) do paths[ep.path] = true end
        -- Anchors are harvested from the loaded snapshot without any request,
        -- so an API-shaped link is real evidence; ordinary pages and
        -- error-reporting/telemetry paths stay filtered.
        assert(paths['/api/items'], 'anchor-derived API path missing')
        assert(paths['/api/first'], 'script-derived API path missing')
        assert(not paths['/dashboard'] and not paths['/js_errors'] and not paths['/telemetry'])
        local linked = 0
        for _, ep in ipairs(first.endpoints) do
            if ep.path == '/api/items' then linked = linked + 1; assert(ep.discovery_method == 'html-link') end
        end
        assert(linked == 1)
        -- api_only=false keeps the whole same-origin candidate surface.
        local wide = qute.scrape(active, {api_only = false})
        local wide_paths = {}
        for _, ep in ipairs(wide.endpoints) do wide_paths[ep.path] = true end
        assert(wide_paths['/dashboard'] and wide_paths['/js_errors'])
        assert(qute.scrape(active, {follow_links = false}).endpoints ~= nil)
        local previous = qute.enrich(active, first.endpoints).schemas
        active:load_html([[<script>fetch('/api/first?offset=3&limit=2.5')</script>
            <form action='/api/first' method='post'><input name='locale'></form>
            <form action='/api/second' method='post'>
            <input name='arrival' type='date' required value='private_form_marker'></form>]], 'https://example.test/next')
        local next_page = qute.enrich(active, qute.scrape(active).endpoints).schemas
        for _, schema in ipairs(next_page) do previous[#previous + 1] = schema end
        local serializer = qute.plugin('schema_grabber')
        local rendered = serializer.serialize(previous, {include_examples=false, redact_secrets=true})
        -- /api/items (anchor), /api/first (script GET + form POST), /api/second.
        assert(rendered.schema_count == 4)
        assert(#qute.json.decode(rendered.postman_json).item == 4)
        local _, gets = rendered.openapi_yaml:gsub('\n    get:', '')
        assert(gets == 2)
        assert(rendered.openapi_yaml:find('/api/items',1,true))
        assert(rendered.openapi_yaml:find('/api/first',1,true))
        assert(rendered.openapi_yaml:find('/api/second',1,true))
        assert(rendered.openapi_yaml:find('customer',1,true))
        assert(rendered.openapi_yaml:find('arrival',1,true))
        assert(rendered.openapi_yaml:find("name: 'offset'",1,true))
        assert(rendered.openapi_yaml:find("name: 'limit'\n          in: query\n          required: false\n          schema:\n            type: number",1,true))
        assert(rendered.openapi_yaml:find('locale',1,true))
        assert(rendered.openapi_yaml:find('evidence-count: 2',1,true))
        -- Serialization must not mutate the original per-page evidence.
        for _, record in ipairs(previous) do assert(record.evidence_count == nil) end
        assert(serializer.serialize(previous, {include_examples=false}).openapi_yaml == rendered.openapi_yaml)
        assert(not rendered.openapi_yaml:find('private_marker',1,true))
        assert(not rendered.postman_json:find('private_form_marker',1,true))
        assert(not pcall(serializer.serialize, {false}))
        assert(not pcall(serializer.serialize, {[2]={}}))
        active:close()
    )LUA");
    EXPECT_TRUE(result.ok) << result.error;
}

TEST(QutebrowserBridge, SchemaCompositionKeepsObservedEvidenceWithoutReprobing) {
    LuaRuntime lua;
    ASSERT_TRUE(setup(lua)) << lua.last_error();
    const auto result = lua.run(R"LUA(
        local active, owner = qute.load_snapshot{url='https://example.test/', html='<main></main>'}
        local grabber = qute.plugin('schema_grabber')
        local calls = 0
        local observed = grabber.enrich({
            document=function() return active:document() end,
            request=function(_, method, url)
                calls = calls + 1
                assert(method == 'GET' and url == 'https://example.test/api/items?offset=2')
                return {status=200, headers={['content-type']='application/json'},
                    body='{"id":42,"token":"private_response_marker"}'}
            end
        }, {endpoints={{url='https://example.test/api/items?offset=2', path='/api/items', method='get'}}})
        assert(calls == 1 and observed.probe_count == 1)
        local placeholder = qute.enrich(active, {{url='https://example.test/api/items?limit=2',
            path='/api/items', method='get'}})
        local original = {observed.schemas[1], placeholder.schemas[1]}
        local rendered = grabber.serialize(original, {redact_secrets=true, include_examples=false})
        assert(calls == 1 and rendered.schema_count == 1)
        assert(rendered.openapi_yaml:find('Observed response',1,true))
        assert(rendered.openapi_yaml:find("response-provenance: 'observed-response'",1,true))
        assert(rendered.openapi_yaml:find('application/json',1,true))
        assert(not rendered.openapi_yaml:find('private_response_marker',1,true))
        assert(not rendered.postman_json:find('private_response_marker',1,true))
        assert(placeholder.schemas[1].responses[1].observed == false)
        active:close()
    )LUA");
    EXPECT_TRUE(result.ok) << result.error;
}
}  // namespace
