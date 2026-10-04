// Plugin lifecycle for the OpenCode bridge. Loading the plugin is
// network-free; all OpenCode IPC happens through explicit OpenCodeClient
// calls over the host NetworkClient.

#include "opencode_bridge.hpp"
#include "prowsetk/ProwseTk-Plugin.h"

#include <mutex>
#include <string>

namespace prowsetk::plugins::opencode_bridge {
namespace {

const char* const kCapabilities[] = {
    "opencode-bridge", "prompt-scraping", "scraper-generation",
    "dom-introspection", "endpoint-relay", "lua-lopencode"};

const ProwseTkPluginInfo kInfo = {
    "opencode-bridge",
    "0.1.0",
    "2",
    "Bi-directional bridge between Flatworm sessions and an OpenCode agent server",
    PROWSETK_PLUGIN_NATIVE,
    "lopencode",
    nullptr,
    kCapabilities,
    6};

struct PluginConfig {
    BridgeConfig bridge = config_from_environment();
};

PluginConfig& config() {
    static PluginConfig instance;
    return instance;
}
std::mutex& config_mutex() {
    static std::mutex mutex;
    return mutex;
}

const char* get_entry(const ProwseTkConfigEntry* entries, std::size_t count, const char* key) {
    for (std::size_t i = 0; i < count; ++i) {
        if (entries[i].key != nullptr && std::string_view(entries[i].key) == key) return entries[i].value;
    }
    return nullptr;
}

int plugin_initialize(ProwseTkHost* host) {
    if (host == nullptr || host->api == nullptr ||
        host->api->abi_version != PROWSETK_PLUGIN_ABI_VERSION) {
        return PROWSETK_STATUS_INVALID_ARGUMENT;
    }
    try {
        std::lock_guard<std::mutex> lock(config_mutex());
        config().bridge = config_from_environment();
        validate_config(config().bridge);
    } catch (...) {
        return PROWSETK_STATUS_INVALID_ARGUMENT;
    }
    if (host->api->log != nullptr) {
        host->api->log(host->api->user_data, PROWSETK_LOG_INFO, "opencode-bridge plugin initialized");
    }
    return PROWSETK_STATUS_OK;
}

void plugin_shutdown(ProwseTkHost* host) {
    try {
        std::lock_guard<std::mutex> lock(config_mutex());
        config() = PluginConfig{};
    } catch (...) {
    }
    if (host != nullptr && host->api != nullptr && host->api->log != nullptr) {
        host->api->log(host->api->user_data, PROWSETK_LOG_INFO, "opencode-bridge plugin shutdown");
    }
}

const ProwseTkPluginInfo* plugin_info() { return &kInfo; }

int plugin_configure(ProwseTkHost*, const ProwseTkConfigEntry* entries, std::size_t count) {
    if (entries == nullptr && count != 0) return PROWSETK_STATUS_INVALID_ARGUMENT;
    try {
        BridgeConfig next = config_from_environment();
        if (const char* v = get_entry(entries, count, "base_url")) next.base_url = v;
        if (const char* v = get_entry(entries, count, "username")) next.username = v;
        if (const char* v = get_entry(entries, count, "password")) next.password = v;
        if (const char* v = get_entry(entries, count, "timeout_ms")) next.timeout_ms = std::stoi(v);
        if (const char* v = get_entry(entries, count, "max_requests")) {
            next.max_requests = static_cast<std::size_t>(std::stoul(v));
        }
        if (const char* v = get_entry(entries, count, "max_input_bytes")) {
            next.max_input_bytes = static_cast<std::size_t>(std::stoul(v));
        }
        if (const char* v = get_entry(entries, count, "max_response_bytes")) {
            next.max_response_bytes = static_cast<std::size_t>(std::stoul(v));
        }
        if (const char* v = get_entry(entries, count, "api_prefix")) next.api_prefix = v;
        if (const char* v = get_entry(entries, count, "prompt_wait_ms")) {
            next.prompt_wait_ms = std::stoi(v);
        }
        if (const char* v = get_entry(entries, count, "allow_remote_http")) {
            const std::string flag = v;
            next.allow_remote_http =
                flag == "true" || flag == "1" || flag == "yes" || flag == "on";
        }
        validate_config(next);
        std::lock_guard<std::mutex> lock(config_mutex());
        config().bridge = std::move(next);
    } catch (...) {
        return PROWSETK_STATUS_INVALID_ARGUMENT;
    }
    return PROWSETK_STATUS_OK;
}

int hook_continue(ProwseTkHookResult* result) {
    if (result == nullptr) return PROWSETK_STATUS_INVALID_ARGUMENT;
    result->action = PROWSETK_HOOK_CONTINUE;
    result->message = nullptr;
    result->replacement_request = nullptr;
    return PROWSETK_STATUS_OK;
}

int plugin_before_request(ProwseTkHost*, const ProwseTkHttpRequest* request, ProwseTkHookResult* result) {
    if (request == nullptr) return PROWSETK_STATUS_INVALID_ARGUMENT;
    return hook_continue(result);
}

int plugin_after_response(ProwseTkHost*, const ProwseTkHttpRequest* request,
                           const ProwseTkHttpResponse* response, ProwseTkHookResult* result) {
    if (request == nullptr || response == nullptr) return PROWSETK_STATUS_INVALID_ARGUMENT;
    return hook_continue(result);
}

int plugin_on_document(ProwseTkHost*, const ProwseTkDocumentSnapshot* document) {
    if (document == nullptr) return PROWSETK_STATUS_INVALID_ARGUMENT;
    // Observation only: no network, no logging of page content.
    return PROWSETK_STATUS_OK;
}

const ProwseTkPlugin kPlugin = {
    plugin_initialize, plugin_shutdown, plugin_info, plugin_configure,
    plugin_before_request, plugin_after_response, plugin_on_document};

}  // namespace
}  // namespace prowsetk::plugins::opencode_bridge

extern "C" PROWSETK_PLUGIN_EXPORT const ProwseTkPlugin* prowsetk_plugin_entry(void) {
    return &prowsetk::plugins::opencode_bridge::kPlugin;
}
