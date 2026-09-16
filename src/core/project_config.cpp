#include "prowsetk/project_config.hpp"

#include <fstream>
#include <sstream>

#include "prowsetk/error.hpp"

#ifdef PROWSETK_HAVE_TOMLPLUSPLUS
#include <toml.hpp>
#endif

namespace prowsetk {
namespace {

#ifdef PROWSETK_HAVE_TOMLPLUSPLUS

const toml::table* find_table(const toml::table& root, const char* key) {
    const auto it = root.find(key);
    if (it == root.end() || !it->second.is_table()) {
        return nullptr;
    }
    return it->second.as_table();
}

std::string table_string(const toml::table& table, const char* key,
                         const std::string& fallback) {
    const auto it = table.find(key);
    if (it == table.end() || !it->second.is_string()) {
        return fallback;
    }
    return it->second.as_string()->get();
}

bool table_bool(const toml::table& table, const char* key, bool fallback) {
    const auto it = table.find(key);
    if (it == table.end() || !it->second.is_boolean()) {
        return fallback;
    }
    return it->second.as_boolean()->get();
}

std::int64_t table_int(const toml::table& table, const char* key,
                       std::int64_t fallback) {
    const auto it = table.find(key);
    if (it == table.end() || !it->second.is_integer()) {
        return fallback;
    }
    return it->second.as_integer()->get();
}

double table_float(const toml::table& table, const char* key, double fallback) {
    const auto it = table.find(key);
    if (it == table.end() || !it->second.is_floating_point()) {
        return fallback;
    }
    return it->second.as_floating_point()->get();
}

std::vector<std::string> table_string_array(const toml::table& table,
                                            const char* key) {
    std::vector<std::string> values;
    const auto it = table.find(key);
    if (it == table.end() || !it->second.is_array()) {
        return values;
    }
    for (const auto& item : *it->second.as_array()) {
        if (item.is_string()) {
            values.push_back(item.as_string()->get());
        }
    }
    return values;
}

std::vector<ConfigArgument> parse_arguments(const toml::array* array) {
    std::vector<ConfigArgument> arguments;
    if (array == nullptr) {
        return arguments;
    }
    for (const auto& item : *array) {
        if (!item.is_table()) {
            continue;
        }
        const auto* table = item.as_table();
        ConfigArgument argument;
        argument.name = table_string(*table, "name", {});
        argument.type = table_string(*table, "type", "string");
        argument.required = table_bool(*table, "required", false);
        argument.secret = table_bool(*table, "secret", false);
        argument.default_value = table_string(*table, "default", {});
        if (!argument.name.empty()) {
            arguments.push_back(std::move(argument));
        }
    }
    return arguments;
}

void parse_drivers(const toml::table& root, ProjectConfig& config) {
    const auto it = root.find("drivers");
    if (it == root.end() || !it->second.is_array()) {
        return;
    }
    for (const auto& item : *it->second.as_array()) {
        if (!item.is_table()) {
            continue;
        }
        const auto* table = item.as_table();
        DriverConfig driver;
        driver.name = table_string(*table, "name", {});
        driver.description = table_string(*table, "description", {});
        driver.script = table_string(*table, "script", {});
        driver.entrypoint = table_string(*table, "entrypoint", "main");
        driver.enabled = table_bool(*table, "enabled", true);
        const auto args = table->find("arguments");
        driver.arguments =
            args != table->end() && args->second.is_array()
                ? parse_arguments(args->second.as_array())
                : std::vector<ConfigArgument>{};
        if (!driver.name.empty()) {
            config.drivers.push_back(std::move(driver));
        }
    }
}

void parse_extensions(const toml::table& root, ProjectConfig& config) {
    const auto it = root.find("extensions");
    if (it == root.end() || !it->second.is_array()) {
        return;
    }
    for (const auto& item : *it->second.as_array()) {
        if (!item.is_table()) {
            continue;
        }
        const auto* table = item.as_table();
        ExtensionConfig extension;
        extension.name = table_string(*table, "name", {});
        extension.description = table_string(*table, "description", {});
        extension.type = table_string(*table, "type", "lua");
        extension.module = table_string(*table, "module", {});
        extension.enabled = table_bool(*table, "enabled", true);
        extension.autoload = table_bool(*table, "autoload", true);
        extension.required_modules = table_string_array(*table, "requires");
        extension.events = table_string_array(*table, "events");
        if (!extension.name.empty()) {
            config.extensions.push_back(std::move(extension));
        }
    }
}

void parse_plugins(const toml::table& root, ProjectConfig& config) {
    const auto it = root.find("plugins");
    if (it == root.end() || !it->second.is_array()) {
        return;
    }
    for (const auto& item : *it->second.as_array()) {
        if (!item.is_table()) {
            continue;
        }
        const auto* table = item.as_table();
        PluginConfig plugin;
        plugin.name = table_string(*table, "name", {});
        plugin.description = table_string(*table, "description", {});
        plugin.type = table_string(*table, "type", {});
        plugin.path = table_string(*table, "path", {});
        plugin.enabled = table_bool(*table, "enabled", true);
        plugin.autoload = table_bool(*table, "autoload", false);
        plugin.capabilities = table_string_array(*table, "capabilities");
        plugin.lua_modules = table_string_array(*table, "lua_modules");
        if (!plugin.name.empty()) {
            config.plugins.push_back(std::move(plugin));
        }
    }
}

void parse_plugin_config(const toml::table& root, ProjectConfig& config) {
    const auto it = root.find("plugin_config");
    if (it == root.end() || !it->second.is_table()) {
        return;
    }
    for (const auto& [key, value] : *it->second.as_table()) {
        if (!value.is_table()) {
            continue;
        }
        const auto* table = value.as_table();
        PluginSandboxConfig sandbox;
        const auto component = table->find("component");
        if (component != table->end() && component->second.is_boolean()) {
            sandbox.component = component->second.as_boolean()->get();
        }
        const auto wasi = table->find("wasi");
        if (wasi != table->end() && wasi->second.is_boolean()) {
            sandbox.wasi = wasi->second.as_boolean()->get();
        }
        const auto memory = table->find("max_memory_mb");
        if (memory != table->end() && memory->second.is_integer()) {
            sandbox.max_memory_mb =
                static_cast<std::size_t>(memory->second.as_integer()->get());
        }
        const auto timeout = table->find("execution_timeout_ms");
        if (timeout != table->end() && timeout->second.is_integer()) {
            sandbox.execution_timeout_ms = static_cast<std::uint32_t>(
                timeout->second.as_integer()->get());
        }
        const auto filesystem = table->find("filesystem");
        if (filesystem != table->end() && filesystem->second.is_string()) {
            sandbox.filesystem = filesystem->second.as_string()->get();
        }
        const auto network = table->find("network");
        if (network != table->end() && network->second.is_string()) {
            sandbox.network = network->second.as_string()->get();
        }
        config.plugin_config[std::string(key)] = std::move(sandbox);
    }
}

void parse_commands(const toml::table& root, ProjectConfig& config) {
    const auto it = root.find("commands");
    if (it == root.end() || !it->second.is_array()) {
        return;
    }
    for (const auto& item : *it->second.as_array()) {
        if (!item.is_table()) {
            continue;
        }
        const auto* table = item.as_table();
        CommandConfig command;
        command.name = table_string(*table, "name", {});
        command.description = table_string(*table, "description", {});
        command.handler = table_string(*table, "handler", {});
        command.driver = table_string(*table, "driver", {});
        command.output = table_string(*table, "output", {});
        const auto args = table->find("arguments");
        command.arguments =
            args != table->end() && args->second.is_array()
                ? parse_arguments(args->second.as_array())
                : std::vector<ConfigArgument>{};
        if (!command.name.empty()) {
            config.commands.push_back(std::move(command));
        }
    }
}

void parse_endpoint_extraction(const toml::table& root,
                               ProjectConfig& config) {
    const auto* table = find_table(root, "endpoint_extraction");
    if (table == nullptr) {
        return;
    }
    auto& endpoint = config.endpoint_extraction;
    endpoint.enabled = table_bool(*table, "enabled", endpoint.enabled);
    endpoint.plugin = table_string(*table, "plugin", endpoint.plugin);
    endpoint.base_urls = table_string_array(*table, "base_urls");
    endpoint.follow_links = table_bool(*table, "follow_links", endpoint.follow_links);
    endpoint.inspect_forms = table_bool(*table, "inspect_forms", endpoint.inspect_forms);
    endpoint.inspect_inline_scripts =
        table_bool(*table, "inspect_inline_scripts", endpoint.inspect_inline_scripts);
    endpoint.inspect_external_scripts = table_bool(
        *table, "inspect_external_scripts", endpoint.inspect_external_scripts);
    endpoint.observe_network =
        table_bool(*table, "observe_network", endpoint.observe_network);
    endpoint.inspect_json_config =
        table_bool(*table, "inspect_json_config", endpoint.inspect_json_config);
    endpoint.max_depth =
        static_cast<std::uint32_t>(table_int(*table, "max_depth", endpoint.max_depth));
    endpoint.max_pages =
        static_cast<std::uint32_t>(table_int(*table, "max_pages", endpoint.max_pages));
    endpoint.max_requests = static_cast<std::uint32_t>(
        table_int(*table, "max_requests", endpoint.max_requests));
    endpoint.same_origin_only =
        table_bool(*table, "same_origin_only", endpoint.same_origin_only);
    endpoint.allowed_hosts = table_string_array(*table, "allowed_hosts");
    endpoint.blocked_path_patterns =
        table_string_array(*table, "blocked_path_patterns");
    endpoint.infer_parameters =
        table_bool(*table, "infer_parameters", endpoint.infer_parameters);
    endpoint.infer_request_bodies = table_bool(
        *table, "infer_request_bodies", endpoint.infer_request_bodies);
    endpoint.infer_response_schemas = table_bool(
        *table, "infer_response_schemas", endpoint.infer_response_schemas);
    endpoint.infer_status_codes =
        table_bool(*table, "infer_status_codes", endpoint.infer_status_codes);
    endpoint.minimum_confidence =
        table_float(*table, "minimum_confidence", endpoint.minimum_confidence);
    endpoint.openapi_version =
        table_string(*table, "openapi_version", endpoint.openapi_version);
    endpoint.output = table_string(*table, "output", endpoint.output);
    endpoint.include_examples =
        table_bool(*table, "include_examples", endpoint.include_examples);
    endpoint.include_provenance =
        table_bool(*table, "include_provenance", endpoint.include_provenance);
    endpoint.redact_cookies =
        table_bool(*table, "redact_cookies", endpoint.redact_cookies);
    endpoint.redact_authorization_headers = table_bool(
        *table, "redact_authorization_headers", endpoint.redact_authorization_headers);
    endpoint.redact_query_parameters =
        table_string_array(*table, "redact_query_parameters");
}

void parse_security(const toml::table& root, ProjectConfig& config) {
    const auto* table = find_table(root, "security");
    if (table == nullptr) {
        return;
    }
    auto& security = config.security;
    const auto schemes = table_string_array(*table, "allowed_schemes");
    if (!schemes.empty()) {
        security.allowed_schemes = schemes;
    }
    security.allow_file_urls = table_bool(*table, "allow_file_urls", security.allow_file_urls);
    security.allow_private_networks =
        table_bool(*table, "allow_private_networks", security.allow_private_networks);
    security.allow_loopback =
        table_bool(*table, "allow_loopback", security.allow_loopback);
    security.allow_native_lua =
        table_bool(*table, "allow_native_lua", security.allow_native_lua);
    security.allow_dynamic_plugin_loading = table_bool(
        *table, "allow_dynamic_plugin_loading", security.allow_dynamic_plugin_loading);
}

void parse_sessions(const toml::table& root, ProjectConfig& config) {
    const auto* table = find_table(root, "sessions");
    if (table == nullptr) {
        return;
    }
    auto& sessions = config.sessions;
    sessions.default_profile =
        table_string(*table, "default_profile", sessions.default_profile);
    sessions.reuse_cookies =
        table_bool(*table, "reuse_cookies", sessions.reuse_cookies);
    sessions.persist_session =
        table_bool(*table, "persist_session", sessions.persist_session);
    sessions.isolate_storage =
        table_bool(*table, "isolate_storage", sessions.isolate_storage);
}

void parse_variables(const toml::table& root, ProjectConfig& config) {
    const auto* table = find_table(root, "variables");
    if (table == nullptr) {
        return;
    }
    for (const auto& [key, value] : *table) {
        if (value.is_string()) {
            config.variables[std::string(key)] = value.as_string()->get();
        }
    }
}

ProjectConfig parse_root(const toml::table& root) {
    ProjectConfig config;

    if (const auto* project = find_table(root, "project")) {
        config.name = table_string(*project, "name", config.name);
        config.version = table_string(*project, "version", config.version);
        config.description =
            table_string(*project, "description", config.description);
        config.root = table_string(*project, "root", config.root);
    }

    if (const auto* engine = find_table(root, "engine")) {
        config.user_agent = table_string(*engine, "user_agent", config.user_agent);
        config.javascript = table_bool(*engine, "javascript", config.javascript);
        config.follow_redirects =
            table_bool(*engine, "follow_redirects", config.follow_redirects);
        config.max_redirects = static_cast<int>(
            table_int(*engine, "max_redirects", config.max_redirects));
        config.timeout_ms =
            static_cast<int>(table_int(*engine, "timeout_ms", config.timeout_ms));
        config.unsupported_api_behavior = table_string(
            *engine, "unsupported_api_behavior", config.unsupported_api_behavior);
        config.observe_network =
            table_bool(*engine, "observe_network", config.observe_network);
    }

    if (const auto* lua = find_table(root, "lua")) {
        config.lua_version = table_string(*lua, "version", config.lua_version);
        const auto libraries = table_string_array(*lua, "libraries");
        if (!libraries.empty()) {
            config.lua_libraries = libraries;
        }
        config.lua_preload = table_string_array(*lua, "preload");
        config.allow_extensions =
            table_bool(*lua, "allow_extensions", config.allow_extensions);
        config.lua_max_memory_mb = static_cast<std::size_t>(
            table_int(*lua, "max_memory_mb",
                      static_cast<std::int64_t>(config.lua_max_memory_mb)));
        config.lua_execution_timeout_ms = static_cast<std::uint32_t>(
            table_int(*lua, "execution_timeout_ms",
                      static_cast<std::int64_t>(config.lua_execution_timeout_ms)));
    }

    if (const auto* network = find_table(root, "network")) {
        config.network_proxy = table_string(*network, "proxy", config.network_proxy);
    }

    parse_drivers(root, config);
    parse_extensions(root, config);
    parse_plugins(root, config);
    parse_plugin_config(root, config);
    parse_commands(root, config);
    parse_endpoint_extraction(root, config);
    parse_security(root, config);
    parse_sessions(root, config);
    parse_variables(root, config);
    return config;
}

#endif  // PROWSETK_HAVE_TOMLPLUSPLUS

}  // namespace

ProjectConfig load_project_config(const std::string& path) {
#ifdef PROWSETK_HAVE_TOMLPLUSPLUS
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw Error(ErrorCode::IoError, "cannot open Prowse.toml: " + path);
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return parse_project_config(buffer.str());
#else
    (void)path;
    throw Error(ErrorCode::Unsupported,
                "Prowse.toml requires a ProwseTk build with tomlplusplus");
#endif
}

ProjectConfig parse_project_config(std::string_view contents) {
#ifdef PROWSETK_HAVE_TOMLPLUSPLUS
    try {
        const toml::table root = toml::parse(contents);
        return parse_root(root);
    } catch (const toml::parse_error& error) {
        throw Error(ErrorCode::ParseError,
                    std::string("invalid Prowse.toml: ") +
                        std::string(error.description()));
    }
#else
    (void)contents;
    throw Error(ErrorCode::Unsupported,
                "Prowse.toml requires a ProwseTk build with tomlplusplus");
#endif
}

}  // namespace prowsetk
