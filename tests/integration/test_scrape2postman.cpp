#include <gtest/gtest.h>
#include "prowsetk/plugins/scrape2postman.hpp"
#include "prowsetk/lua_runtime.hpp"
#include "prowsetk/network_client.hpp"
#include "prowsetk/ProwseTk-Plugin.h"
#include <fstream>

namespace postman = prowsetk::plugins::scrape2postman;
extern "C" const ProwseTkPlugin* prowsetk_plugin_entry(void);

TEST(PostmanIntegration, OfflineAndEmptySessionsDoNotRequestNetwork) {
    prowsetk::Browser browser;
    auto network = std::make_unique<prowsetk::MemoryNetworkClient>();
    auto* observed = network.get();
    browser.set_network_client(std::move(network));
    auto session = browser.create_session();
    EXPECT_TRUE(postman::scrape_from_session(*session).endpoints.empty());
    postman::Scrape2PostmanOptions options;
    options.html = "<script>fetch('/api/users')</script>";
    options.base_url = "https://example.test/";
    auto result = postman::scrape(*session, options);
    EXPECT_EQ(result.endpoints.size(), 1u);
    EXPECT_TRUE(observed->requests().empty());
}

TEST(PostmanIntegration, ResolvesJsonLinksThroughSessionWithBudget) {
    prowsetk::Browser browser;
    auto network = std::make_unique<prowsetk::MemoryNetworkClient>();
    auto* observed = network.get();
    network->set_handler([](const prowsetk::HttpRequest& request) {
        prowsetk::HttpResponse response;
        response.status = 200;
        response.final_url = request.url;
        response.headers = {{"Content-Type", "application/json"}};
        response.body = R"({"next":"/api/next","password":"private-body"})";
        return response;
    });
    browser.set_network_client(std::move(network));
    auto session = browser.create_session();
    session->load_html("<script>fetch('/api/start')</script>", "https://example.test/");
    postman::Scrape2PostmanOptions options;
    options.resolve_chain = true;
    options.max_resolve_requests = 2;
    auto result = postman::scrape_from_session(*session, options);
    EXPECT_EQ(observed->requests().size(), 2u);
    EXPECT_NE(result.postman_json.find("/api/next"), std::string::npos);
    EXPECT_EQ(result.postman_json.find("private-body"), std::string::npos);
    EXPECT_NE(result.postman_json.find("Observed HTTP status: 200"), std::string::npos);
}

TEST(PostmanIntegration, NativeRegistryExportsOnDocument) {
    prowsetk::Browser browser;
    const auto path = std::filesystem::path(TEST_BINARY_DIR) / "native.postman_collection.json";
    browser.plugins().load_native(POSTMAN_PLUGIN_PATH);
    ASSERT_EQ(browser.plugins().initialize_all(), 1u);
    ASSERT_EQ(browser.plugins().configure_all({{"scrape2postman.output", path.string()},
        {"scrape2postman.collection_name", "Native collection"}}), 1u);
    auto session = browser.create_session();
    session->load_html("<script>fetch('/api/users?token=private-token')</script>", "https://example.test/");
    std::ifstream file(path);
    ASSERT_TRUE(file.good());
    const std::string json(std::istreambuf_iterator<char>(file), {});
    EXPECT_NE(json.find("Native collection"), std::string::npos);
    EXPECT_NE(json.find("/api/users"), std::string::npos);
    EXPECT_EQ(json.find("private-token"), std::string::npos);
}

TEST(PostmanIntegration, NativeAbiValidatesInputsAndContainsErrors) {
    const auto* plugin = prowsetk_plugin_entry();
    EXPECT_NE(plugin->initialize(nullptr), 0);
    ProwseTkHostApi api{};
    ProwseTkHost host{&api};
    EXPECT_NE(plugin->initialize(&host), 0);
    api.abi_version = PROWSETK_PLUGIN_ABI_VERSION;
    ASSERT_EQ(plugin->initialize(&host), 0);
    EXPECT_NE(plugin->configure(&host, nullptr, 1), 0);
    ProwseTkConfigEntry invalid{"scrape2postman.include_provenance", "invalid"};
    EXPECT_NE(plugin->configure(&host, &invalid, 1), 0);
    ProwseTkConfigEntry output{"scrape2postman.output", TEST_BINARY_DIR};
    EXPECT_EQ(plugin->configure(&host, &output, 1), 0);
    ProwseTkDocumentSnapshot document{"https://example.test/", "", "<p>test</p>", "test"};
    EXPECT_NE(plugin->on_document(&host, &document), 0);
    plugin->shutdown(&host);
    EXPECT_NE(plugin->on_document(&host, &document), 0);
}

TEST(PostmanIntegration, LuaScrapesWritesAndExposesContentTypes) {
    if (!prowsetk::LuaRuntime::available()) GTEST_SKIP() << "Lua unavailable";
    prowsetk::LuaRuntime lua;
    const auto setup = lua.run("package.path = "
        "'" PROWSETK_SOURCE_DIR "/?.lua;' .. package.path");
    ASSERT_TRUE(setup.ok) << lua.last_error();
    const auto result = lua.run(R"LUA(
        local postman = require("plugins.scrape2postman.lua.scrape2postman")
        local session = require("lprowse").browser.new():create_session()
        local spec = {
            html = '<form action="/api/login" method="post"><input name="password" value="private-form"></form>',
            base_url = "https://example.test/", collection_name = 'Lua "collection"',
            output = ")LUA" TEST_BINARY_DIR R"LUA(/lua.postman_collection.json"
        }
        local result = postman.scrape(session, spec)
        assert(result.endpoint_count == 1)
        assert(result.endpoints[1].request_content_type == "application/x-www-form-urlencoded")
        assert(type(result.endpoints[1].response_content_type) == "string")
        assert(result.postman_json:find('"mode":"urlencoded"', 1, true))
        assert(not result.postman_json:find("private-form", 1, true))
        local file = assert(io.open(spec.output, "rb"))
        assert(file:read("a") == result.postman_json)
        file:close()
        result:write_postman_json(spec.output)
        spec.output = nil
        assert(postman.dump(session:document(), spec) == result.postman_json)
        assert(not pcall(function() result:write_postman_json(")LUA" TEST_BINARY_DIR R"LUA(") end))
    )LUA");
    EXPECT_TRUE(result.ok) << lua.last_error();
}

TEST(PostmanIntegration, LuaRedactsAndSortsWithoutMutatingSpec) {
    if (!prowsetk::LuaRuntime::available()) GTEST_SKIP() << "Lua unavailable";
    prowsetk::LuaRuntime lua;
    ASSERT_TRUE(lua.run("package.path = '" PROWSETK_SOURCE_DIR "/?.lua;' .. package.path").ok);
    const auto result = lua.run(R"LUA(
        local postman = require("plugins.scrape2postman.lua.scrape2postman")
        local a = {url="https://user:private-pass@example.test/api/a?%74oken=private-token#private-fragment",
            source="https://example.test/?password=private-source", method="post", confidence=0.6,
            redirect_chain={"https://example.test/?apikey=private-hop"}, body="private-body",
            resolve_error="private-error"}
        local b = {url="https://other.test/api/a", method="get", parameters={"q"}}
        local spec = {collection_name='Escapes\n"\\' .. string.char(0)}
        local json = postman.render_postman_json({b,a,a}, spec)
        assert(json == postman.render_postman_json({a,b}, spec))
        assert(not json:find("private-", 1, true))
        assert(json:find("q=", 1, true))
        assert(json:find("\\u0000", 1, true))
        spec.include_provenance = false
        assert(not postman.render_postman_json({a},spec):find("Source:",1,true))
        spec.redact_secrets = false
        assert(postman.render_postman_json({a},spec):find("private-token",1,true))
    )LUA");
    EXPECT_TRUE(result.ok) << lua.last_error();
}
