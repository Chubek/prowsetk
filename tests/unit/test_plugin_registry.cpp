#include <gtest/gtest.h>

#include "prowsetk/error.hpp"
#include "prowsetk/plugin_registry.hpp"

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
    EXPECT_EQ(descriptor.abi_version, "1");
    ASSERT_EQ(descriptor.capabilities.size(), 2u);
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
