#include <gtest/gtest.h>

#include "prowsetk/error.hpp"
#include "prowsetk/project_config.hpp"

#ifdef PROWSETK_HAVE_TOMLPLUSPLUS
#define PROWSETK_CONFIG_TESTS_ENABLED 1
#else
#define PROWSETK_CONFIG_TESTS_ENABLED 0
#endif

using prowsetk::parse_project_config;

TEST(ProjectConfig, ParsesProjectAndEngineSections) {
    const auto config = parse_project_config(R"(
[project]
name = "demo"
version = "0.2.0"
description = "Demo project"

[engine]
javascript = true
follow_redirects = true
max_redirects = 5
timeout_ms = 15000
user_agent = "DemoAgent/1.0"
unsupported_api_behavior = "exception"
observe_network = true
)");
    EXPECT_EQ(config.name, "demo");
    EXPECT_EQ(config.version, "0.2.0");
    EXPECT_TRUE(config.javascript);
    EXPECT_EQ(config.max_redirects, 5);
    EXPECT_EQ(config.timeout_ms, 15000);
    EXPECT_EQ(config.user_agent, "DemoAgent/1.0");
    EXPECT_EQ(config.unsupported_api_behavior, "exception");
    EXPECT_TRUE(config.observe_network);
}

TEST(ProjectConfig, ParsesDriversAndArguments) {
    const auto config = parse_project_config(R"(
[[drivers]]
name = "crawl"
description = "Crawl the site"
script = "drivers/crawl.lua"
entrypoint = "main"
enabled = true

[[drivers.arguments]]
name = "url"
type = "string"
required = true

[[drivers.arguments]]
name = "depth"
type = "integer"
required = false
default = "3"
)");
    ASSERT_EQ(config.drivers.size(), 1u);
    const auto& driver = config.drivers[0];
    EXPECT_EQ(driver.name, "crawl");
    EXPECT_EQ(driver.script, "drivers/crawl.lua");
    EXPECT_EQ(driver.entrypoint, "main");
    ASSERT_EQ(driver.arguments.size(), 2u);
    EXPECT_EQ(driver.arguments[0].name, "url");
    EXPECT_TRUE(driver.arguments[0].required);
    EXPECT_EQ(driver.arguments[1].default_value, "3");
}

TEST(ProjectConfig, ParsesPluginsAndPerPluginSandbox) {
    const auto config = parse_project_config(R"(
[[plugins]]
name = "endpoint-extraction"
type = "native"
path = "plugins/libextract.so"
enabled = true
autoload = true
capabilities = ["endpoint-discovery", "openapi-yaml"]
lua_modules = ["lprowsext"]

[plugin_config.endpoint-extraction]
component = true
wasi = false
max_memory_mb = 128
execution_timeout_ms = 10000
)");
    ASSERT_EQ(config.plugins.size(), 1u);
    EXPECT_EQ(config.plugins[0].name, "endpoint-extraction");
    EXPECT_EQ(config.plugins[0].type, "native");
    ASSERT_EQ(config.plugins[0].capabilities.size(), 2u);

    const auto it = config.plugin_config.find("endpoint-extraction");
    ASSERT_NE(it, config.plugin_config.end());
    EXPECT_TRUE(it->second.component.value());
    EXPECT_FALSE(it->second.wasi.value());
    EXPECT_EQ(it->second.max_memory_mb.value(), 128u);
    EXPECT_EQ(it->second.execution_timeout_ms.value(), 10000u);
}

TEST(ProjectConfig, ParsesSecuritySessionsAndEndpointDefaults) {
    const auto config = parse_project_config(R"(
[security]
allowed_schemes = ["https"]
allow_file_urls = false
allow_private_networks = false

[sessions]
default_profile = "scan"
reuse_cookies = true
persist_session = true

[endpoint_extraction]
enabled = true
max_depth = 4
max_pages = 50
minimum_confidence = 0.4
redact_authorization_headers = true
redact_query_parameters = ["token", "api_key"]
)");
    ASSERT_EQ(config.security.allowed_schemes.size(), 1u);
    EXPECT_EQ(config.security.allowed_schemes[0], "https");
    EXPECT_EQ(config.sessions.default_profile, "scan");
    EXPECT_TRUE(config.sessions.reuse_cookies);
    EXPECT_TRUE(config.sessions.persist_session);
    EXPECT_EQ(config.endpoint_extraction.max_depth, 4u);
    EXPECT_EQ(config.endpoint_extraction.max_pages, 50u);
    EXPECT_DOUBLE_EQ(config.endpoint_extraction.minimum_confidence, 0.4);
    EXPECT_TRUE(config.endpoint_extraction.redact_authorization_headers);
    ASSERT_EQ(config.endpoint_extraction.redact_query_parameters.size(), 2u);
    EXPECT_EQ(config.endpoint_extraction.redact_query_parameters[0], "token");
}

TEST(ProjectConfig, UnknownKeysAreIgnored) {
    const auto config = parse_project_config(R"(
[engine]
javascript = true
future_engine_flag = 42

[[drivers]]
name = "d"
script = "d.lua"
unknown_driver_field = "ignored"
)");
    EXPECT_TRUE(config.javascript);
    ASSERT_EQ(config.drivers.size(), 1u);
    EXPECT_EQ(config.drivers[0].name, "d");
}

TEST(ProjectConfig, MalformedTomlThrowsParseError) {
    EXPECT_THROW(parse_project_config("this is [not toml"), prowsetk::Error);
}

#if !PROWSETK_CONFIG_TESTS_ENABLED
TEST(ProjectConfig, UnsupportedWithoutTomlplusplus) {
    EXPECT_THROW(parse_project_config("[project]\nname = 'x'\n"),
                 prowsetk::Error);
}
#endif