#include <gtest/gtest.h>

#include "prowsetk/browser.hpp"
#include "prowsetk/error.hpp"
#include "prowsetk/network_client.hpp"
#include "prowsetk/plugin_registry.hpp"

#include <tuple>

using prowsetk::Error;
using prowsetk::PluginRegistry;

#ifndef TEST_PLUGIN_PATH
#define TEST_PLUGIN_PATH ""
#endif

namespace {
struct LogRecord {
    int last_level = -1;
    std::string last_message;
};

void log_sink(void* user_data, int level, const char* message) {
    auto* record = static_cast<LogRecord*>(user_data);
    record->last_level = level;
    record->last_message = message != nullptr ? message : "";
}
}  // namespace

TEST(PluginRegistry, LoadsNativePluginMetadata) {
    PluginRegistry registry;
    const auto& descriptor = registry.load_native(TEST_PLUGIN_PATH);
    EXPECT_EQ(descriptor.name, "test-plugin");
    EXPECT_EQ(descriptor.version, "1.0.0");
    EXPECT_EQ(descriptor.abi_version, "2");
    EXPECT_EQ(descriptor.lua_module, "test_plugin_lua");
    EXPECT_EQ(descriptor.wasm_world, "prowsetk-plugin");
    ASSERT_EQ(descriptor.capabilities.size(), 3u);
    EXPECT_EQ(descriptor.capabilities[0], "document-extractor");
}

TEST(PluginRegistry, InitializesAndShutsDown) {
    PluginRegistry registry;
    registry.load_native(TEST_PLUGIN_PATH);
    LogRecord record;
    registry.set_log_sink(log_sink, &record);

    EXPECT_EQ(registry.initialize_all(), 1u);
    const auto* descriptor = registry.find("test-plugin");
    ASSERT_NE(descriptor, nullptr);
    EXPECT_TRUE(descriptor->initialized);
    EXPECT_EQ(record.last_level, PROWSETK_LOG_INFO);
    EXPECT_NE(record.last_message.find("test-plugin"), std::string::npos);

    registry.shutdown_all();
    EXPECT_EQ(registry.plugins().size(), 0u);
}

TEST(PluginRegistry, ConfiguresInitializedNativePlugins) {
    PluginRegistry registry;
    registry.load_native(TEST_PLUGIN_PATH);
    EXPECT_EQ(registry.initialize_all(), 1u);
    EXPECT_EQ(registry.configure_all({{"mode", "strict"}}), 1u);
}

TEST(PluginRegistry, TracksLuaAndWasmPlugins) {
    PluginRegistry registry;
    registry.load_lua("extensions/helpers.lua");
    prowsetk::WasmSandboxConfig config;
    config.max_memory_bytes = 64u * 1024u * 1024u;
    registry.load_wasm("plugins/extractor.wasm", config);

    const auto plugins = registry.plugins();
    ASSERT_EQ(plugins.size(), 2u);
    EXPECT_EQ(plugins[0].type, PROWSETK_PLUGIN_LUA);
    EXPECT_EQ(plugins[1].type, PROWSETK_PLUGIN_WASM);
    EXPECT_EQ(plugins[0].name, "helpers");
}

TEST(PluginRegistry, MissingFileThrows) {
    PluginRegistry registry;
    EXPECT_THROW(registry.load_native("/nonexistent/plugin.so"), Error);
}

TEST(PluginRegistry, EmitsLifecycleEvents) {
    PluginRegistry registry;
    registry.load_native(TEST_PLUGIN_PATH);
    prowsetk::EventDispatcher dispatcher;
    registry.set_event_dispatcher(&dispatcher);

    std::vector<prowsetk::EventType> types;
    std::vector<std::string> names;
    dispatcher.subscribe_all([&](prowsetk::Event& event) {
        types.push_back(event.type);
        names.push_back(event.name);
    });

    EXPECT_EQ(registry.initialize_all(), 1u);
    registry.shutdown_all();

    ASSERT_EQ(types.size(), 2u);
    EXPECT_EQ(types[0], prowsetk::EventType::PluginInit);
    EXPECT_EQ(types[1], prowsetk::EventType::PluginShutdown);
    EXPECT_EQ(names[0], "test-plugin");
    EXPECT_EQ(names[1], "test-plugin");
}

TEST(PluginRegistry, BeforeRequestHookCanReplaceRequest) {
    prowsetk::Browser browser;
    auto network = std::make_unique<prowsetk::MemoryNetworkClient>();
    std::string seen_url;
    std::string seen_header;
    network->set_handler([&](const prowsetk::HttpRequest& request) {
        seen_url = request.url;
        for (const auto& [name, value] : request.headers) {
            if (name == "X-Plugin") {
                seen_header = value;
            }
        }
        prowsetk::HttpResponse response;
        response.status = 200;
        response.body = "ok";
        response.final_url = request.url;
        return response;
    });
    browser.set_network_client(std::move(network));
    browser.plugins().load_native(TEST_PLUGIN_PATH);
    ASSERT_EQ(browser.plugins().initialize_all(), 1u);

    auto session = browser.create_session();
    prowsetk::HttpRequest request;
    request.method = "GET";
    request.url = "https://original.example/replace";
    const auto response = session->request(request);

    EXPECT_EQ(response.status, 200);
    EXPECT_EQ(seen_url, "https://rewritten.example/final");
    EXPECT_EQ(seen_header, "rewritten");
}

TEST(PluginRegistry, BeforeRequestHookCanRejectRequest) {
    prowsetk::Browser browser;
    browser.plugins().load_native(TEST_PLUGIN_PATH);
    ASSERT_EQ(browser.plugins().initialize_all(), 1u);

    auto session = browser.create_session();
    prowsetk::HttpRequest request;
    request.method = "GET";
    request.url = "https://example.test/blocked";
    EXPECT_THROW(session->request(request), Error);
}

TEST(PluginRegistry, AfterResponseHookCanRejectResponse) {
    prowsetk::Browser browser;
    auto network = std::make_unique<prowsetk::MemoryNetworkClient>();
    network->set_handler([](const prowsetk::HttpRequest& request) {
        prowsetk::HttpResponse response;
        response.status = 451;
        response.body = "blocked";
        response.final_url = request.url;
        return response;
    });
    browser.set_network_client(std::move(network));
    browser.plugins().load_native(TEST_PLUGIN_PATH);
    ASSERT_EQ(browser.plugins().initialize_all(), 1u);

    auto session = browser.create_session();
    prowsetk::HttpRequest request;
    request.method = "GET";
    request.url = "https://example.test/legal";
    EXPECT_THROW(session->request(request), Error);
}

TEST(PluginRegistry, DocumentHookReceivesSnapshotAndCanEmitHostEvent) {
    prowsetk::Browser browser;
    browser.plugins().load_native(TEST_PLUGIN_PATH);
    ASSERT_EQ(browser.plugins().initialize_all(), 1u);

    std::vector<prowsetk::Event> console_events;
    browser.events().subscribe(prowsetk::EventType::Console,
                               [&](prowsetk::Event& event) {
                                   console_events.push_back(event);
                               });

    auto session = browser.create_session();
    session->load_html("<html><head><title>Snapshot</title></head>"
                       "<body><p>Body</p></body></html>",
                       "https://example.test/");

    ASSERT_FALSE(console_events.empty());
    EXPECT_EQ(console_events.back().name, "test-plugin");
    EXPECT_EQ(console_events.back().message, "Snapshot");
}

class PluginRegistryLuaDescriptorCases
    : public testing::TestWithParam<std::tuple<std::string, std::string>> {};

TEST_P(PluginRegistryLuaDescriptorCases, DerivesLuaDescriptorFromPath) {
    const auto& [path, expected_name] = GetParam();
    PluginRegistry registry;

    const auto& descriptor = registry.load_lua(path);

    EXPECT_EQ(descriptor.name, expected_name);
    EXPECT_EQ(descriptor.path, path);
    EXPECT_EQ(descriptor.type, PROWSETK_PLUGIN_LUA);
    EXPECT_FALSE(descriptor.initialized);
    EXPECT_TRUE(descriptor.capabilities.empty());
}

INSTANTIATE_TEST_SUITE_P(
    PluginRegistryPathCases, PluginRegistryLuaDescriptorCases,
    testing::Values(
        std::make_tuple("plugins/auth.lua", "auth"),
        std::make_tuple("plugins/forms/login.lua", "login"),
        std::make_tuple("drivers/extract-links.lua", "extract-links"),
        std::make_tuple("relative/module.name.lua", "module.name"),
        std::make_tuple("/opt/prowsetk/extensions/cookies.lua", "cookies"),
        std::make_tuple("simple.lua", "simple"),
        std::make_tuple("nested.with.dots/filter.lua", "filter"),
        std::make_tuple("plugins/_private.lua", "_private"),
        std::make_tuple("plugins/api_v2.lua", "api_v2"),
        std::make_tuple("plugins/x.lua", "x"),
        std::make_tuple("plugins/UPPER.lua", "UPPER"),
        std::make_tuple("plugins/mixed-Case.lua", "mixed-Case"),
        std::make_tuple("plugins/01-bootstrap.lua", "01-bootstrap"),
        std::make_tuple("plugins/session.store.lua", "session.store"),
        std::make_tuple("plugins/http_hooks.lua", "http_hooks"),
        std::make_tuple("plugins/openapi.yaml.lua", "openapi.yaml"),
        std::make_tuple("./local/plugin.lua", "plugin"),
        std::make_tuple("../shared/common.lua", "common"),
        std::make_tuple("plugins/a.b.c.lua", "a.b.c"),
        std::make_tuple("plugins/no_extension", "no_extension")));

class PluginRegistryWasmDescriptorCases
    : public testing::TestWithParam<std::tuple<std::string, std::string,
                                              std::size_t, bool>> {};

TEST_P(PluginRegistryWasmDescriptorCases, DerivesWasmDescriptorFromPath) {
    const auto& [path, expected_name, max_memory, wasi_enabled] = GetParam();
    PluginRegistry registry;
    prowsetk::WasmSandboxConfig config;
    config.max_memory_bytes = max_memory;
    config.wasi = wasi_enabled;

    const auto& descriptor = registry.load_wasm(path, config);

    EXPECT_EQ(descriptor.name, expected_name);
    EXPECT_EQ(descriptor.path, path);
    EXPECT_EQ(descriptor.type, PROWSETK_PLUGIN_WASM);
    EXPECT_FALSE(descriptor.initialized);
    EXPECT_TRUE(descriptor.capabilities.empty());
}

INSTANTIATE_TEST_SUITE_P(
    PluginRegistryPathCases, PluginRegistryWasmDescriptorCases,
    testing::Values(
        std::make_tuple("plugins/extractor.wasm", "extractor", 65536u, false),
        std::make_tuple("plugins/auth-filter.wasm", "auth-filter", 131072u, false),
        std::make_tuple("plugins/audit.wasm", "audit", 262144u, false),
        std::make_tuple("plugins/forms/login.wasm", "login", 524288u, false),
        std::make_tuple("plugins/session.store.wasm", "session.store", 1048576u, false),
        std::make_tuple("/opt/prowsetk/plugins/headers.wasm", "headers", 2097152u, false),
        std::make_tuple("relative/module.name.wasm", "module.name", 4194304u, false),
        std::make_tuple("plugins/UPPER.wasm", "UPPER", 8388608u, false),
        std::make_tuple("plugins/01-bootstrap.wasm", "01-bootstrap", 16777216u, false),
        std::make_tuple("plugins/api_v2.wasm", "api_v2", 33554432u, false),
        std::make_tuple("plugins/x.wasm", "x", 67108864u, false),
        std::make_tuple("plugins/_private.wasm", "_private", 134217728u, false),
        std::make_tuple("./local/plugin.wasm", "plugin", 65536u, true),
        std::make_tuple("../shared/common.wasm", "common", 131072u, true),
        std::make_tuple("plugins/openapi.yaml.wasm", "openapi.yaml", 262144u, false),
        std::make_tuple("plugins/a.b.c.wasm", "a.b.c", 524288u, false),
        std::make_tuple("plugins/no_extension", "no_extension", 1048576u, false),
        std::make_tuple("plugins/mixed-Case.wasm", "mixed-Case", 2097152u, false),
        std::make_tuple("plugins/network-hook.wasm", "network-hook", 4194304u, false),
        std::make_tuple("plugins/document_export.wasm", "document_export", 8388608u, false)));

class PluginRegistryCapabilityCases
    : public testing::TestWithParam<std::tuple<std::string, bool>> {};

TEST_P(PluginRegistryCapabilityCases, ReportsCapabilitiesExactly) {
    const auto& [capability, expected] = GetParam();
    PluginRegistry registry;
    registry.load_native(TEST_PLUGIN_PATH);

    EXPECT_EQ(registry.has_capability(capability), expected);
}

INSTANTIATE_TEST_SUITE_P(
    PluginRegistryCapabilityMatrix, PluginRegistryCapabilityCases,
    testing::Values(
        std::make_tuple("document-extractor", true),
        std::make_tuple("openapi-yaml", true),
        std::make_tuple("network-hook", true),
        std::make_tuple("Document-Extractor", false),
        std::make_tuple("openapi-json", false),
        std::make_tuple("network", false),
        std::make_tuple("lua", false),
        std::make_tuple("wasm", false),
        std::make_tuple("endpoint-extraction", false),
        std::make_tuple("", false),
        std::make_tuple("document-extractor ", false),
        std::make_tuple(" openapi-yaml", false),
        std::make_tuple("network-hook-extra", false),
        std::make_tuple("DOCUMENT-EXTRACTOR", false),
        std::make_tuple("document", false)));

class PluginRegistryRequestPassthroughCases
    : public testing::TestWithParam<std::tuple<std::string, std::string,
                                              std::string, std::string>> {};

TEST_P(PluginRegistryRequestPassthroughCases, LeavesUnmatchedRequestsIntact) {
    const auto& [method, url, body, header_value] = GetParam();
    prowsetk::Browser browser;
    auto network = std::make_unique<prowsetk::MemoryNetworkClient>();
    network->set_handler([&](const prowsetk::HttpRequest& request) {
        EXPECT_EQ(request.method, method);
        EXPECT_EQ(request.url, url);
        EXPECT_EQ(request.body, body);
        bool saw_header = false;
        for (const auto& [name, value] : request.headers) {
            if (name == "X-Case") {
                saw_header = true;
                EXPECT_EQ(value, header_value);
            }
        }
        EXPECT_TRUE(saw_header);
        prowsetk::HttpResponse response;
        response.status = 200;
        response.body = "ok";
        response.final_url = request.url;
        return response;
    });
    browser.set_network_client(std::move(network));
    browser.plugins().load_native(TEST_PLUGIN_PATH);
    ASSERT_EQ(browser.plugins().initialize_all(), 1u);

    auto session = browser.create_session();
    prowsetk::HttpRequest request;
    request.method = method;
    request.url = url;
    request.body = body;
    request.headers.emplace_back("X-Case", header_value);
    const auto response = session->request(request);

    EXPECT_EQ(response.status, 200);
}

INSTANTIATE_TEST_SUITE_P(
    PluginRegistryNetworkCases, PluginRegistryRequestPassthroughCases,
    testing::Values(
        std::make_tuple("GET", "https://example.test/plain", "", "plain"),
        std::make_tuple("POST", "https://example.test/forms", "name=value", "form"),
        std::make_tuple("PUT", "https://example.test/items/1", "{\"a\":1}", "json"),
        std::make_tuple("PATCH", "https://example.test/items/2", "delta", "patch"),
        std::make_tuple("DELETE", "https://example.test/items/3", "", "delete"),
        std::make_tuple("HEAD", "https://example.test/head", "", "head"),
        std::make_tuple("OPTIONS", "https://example.test/options", "", "options"),
        std::make_tuple("GET", "https://example.test/query?ok=yes", "", "query"),
        std::make_tuple("POST", "https://example.test/upload", "payload", "upload"),
        std::make_tuple("GET", "https://sub.example.test/path", "", "subdomain"),
        std::make_tuple("GET", "https://example.test/path.with.dots", "", "dots"),
        std::make_tuple("POST", "https://example.test/api/v2/items", "[]", "array"),
        std::make_tuple("GET", "http://example.test/insecure", "", "http"),
        std::make_tuple("POST", "http://example.test/insecure-form", "a=b", "http-post"),
        std::make_tuple("GET", "https://example.test/case-sensitive", "", "case")));
