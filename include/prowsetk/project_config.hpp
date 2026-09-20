#ifndef PROWSETK_PROJECT_CONFIG_HPP
#define PROWSETK_PROJECT_CONFIG_HPP

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace prowsetk {

// A single declared argument for a driver or command (README `Prowse.toml`).
struct ConfigArgument {
    std::string name;
    std::string type = "string";
    bool required = false;
    bool secret = false;
    std::string default_value;
};

// [[drivers]] — a Lua driver script that controls one or more sessions.
struct DriverConfig {
    std::string name;
    std::string description;
    std::string script;
    std::string entrypoint = "main";
    bool enabled = true;
    std::vector<ConfigArgument> arguments;
};

// [[extensions]] — a Lua extension module.
struct ExtensionConfig {
    std::string name;
    std::string description;
    std::string type = "lua";
    std::string module;
    bool enabled = true;
    bool autoload = true;
    std::vector<std::string> required_modules;
    std::vector<std::string> events;
};

// [[plugins]] — a native, Lua, or WASM plugin.
struct PluginConfig {
    std::string name;
    std::string description;
    std::string type;
    std::string path;
    bool enabled = true;
    bool autoload = false;
    std::vector<std::string> capabilities;
    std::vector<std::string> lua_modules;
};

// Per-plugin sandbox and capability settings (`[plugin_config.<name>]`).
struct PluginSandboxConfig {
    std::optional<bool> component;
    std::optional<bool> wasi;
    std::optional<std::size_t> max_memory_mb;
    std::optional<std::uint32_t> execution_timeout_ms;
    std::optional<std::string> filesystem;
    std::optional<std::string> network;
};

// [[commands]] — a special command exposed by the CLI.
struct CommandConfig {
    std::string name;
    std::string description;
    std::string handler;
    std::string driver;
    std::string output;
    std::vector<ConfigArgument> arguments;
};

// `[endpoint_extraction]` — heuristic endpoint discovery configuration.
struct EndpointExtractionConfig {
    bool enabled = true;
    std::string plugin = "endpoint-extraction";
    std::vector<std::string> base_urls;
    bool follow_links = true;
    bool inspect_forms = true;
    bool inspect_inline_scripts = true;
    bool inspect_external_scripts = true;
    bool observe_network = true;
    bool inspect_json_config = true;
    std::uint32_t max_depth = 2;
    std::uint32_t max_pages = 100;
    std::uint32_t max_requests = 500;
    bool same_origin_only = true;
    std::vector<std::string> allowed_hosts;
    std::vector<std::string> blocked_path_patterns;
    bool infer_parameters = true;
    bool infer_request_bodies = true;
    bool infer_response_schemas = true;
    bool infer_status_codes = true;
    double minimum_confidence = 0.50;
    std::string openapi_version = "3.1.0";
    std::string output = "build/openapi.yaml";
    bool include_examples = true;
    bool include_provenance = true;
    bool redact_cookies = true;
    bool redact_authorization_headers = true;
    std::vector<std::string> redact_query_parameters;
};

// Security policy (`[security]`).
struct SecurityConfig {
    std::vector<std::string> allowed_schemes = {"https", "http"};
    bool allow_file_urls = false;
    bool allow_private_networks = false;
    bool allow_loopback = false;
    bool allow_native_lua = true;
    bool allow_dynamic_plugin_loading = false;
};

// Session defaults (`[sessions]`).
struct SessionDefaultsConfig {
    std::string default_profile = "default";
    bool reuse_cookies = false;
    bool persist_session = false;
    bool isolate_storage = true;
    std::string cookies_json;
};

// Optional user-assisted browser handoff for pages that require interaction.
struct AssistantBrowserConfig {
    bool enabled = false;
    std::string command = "assistant-browser";
    std::string method = "webdriver";
    std::string endpoint;
    std::uint32_t debug_port = 0;
    std::uint32_t wait_timeout_ms = 300000;
};

// The parsed `Prowse.toml` project configuration. Only documented sections are
// read; unknown keys are ignored so forward-compatible configs keep working.
struct ProjectConfig {
    std::string name;
    std::string version;
    std::string description;
    std::string root = ".";

    std::string user_agent;
    bool javascript = true;
    bool follow_redirects = true;
    int max_redirects = 10;
    int timeout_ms = 30000;
    std::string unsupported_api_behavior = "warn";
    bool observe_network = false;

    std::string lua_version = "5.4";
    std::vector<std::string> lua_libraries = {"lprowse", "lprowsext"};
    std::vector<std::string> lua_preload;
    bool allow_extensions = true;
    std::size_t lua_max_memory_mb = 128;
    std::uint32_t lua_execution_timeout_ms = 30000;

    std::string network_proxy;

    std::vector<DriverConfig> drivers;
    std::vector<ExtensionConfig> extensions;
    std::vector<PluginConfig> plugins;
    std::map<std::string, PluginSandboxConfig> plugin_config;
    std::vector<CommandConfig> commands;

    EndpointExtractionConfig endpoint_extraction;
    SecurityConfig security;
    SessionDefaultsConfig sessions;
    AssistantBrowserConfig assistant_browser;
    std::map<std::string, std::string> variables;
};

// Parses `Prowse.toml` from `path`. Throws Error(ParseError) when the file is
// missing, unreadable, or invalid TOML. When ProwseTk is built without
// tomlplusplus, throws Error(Unsupported).
ProjectConfig load_project_config(const std::string& path);

// Parses a `Prowse.toml` document from an in-memory string.
ProjectConfig parse_project_config(std::string_view contents);

}  // namespace prowsetk

#endif  // PROWSETK_PROJECT_CONFIG_HPP
