#include "prowsetk/ProwseTk-Plugin.h"
#include "prowsetk/plugins/beacon.hpp"

#include <cstddef>
#include <cstring>
#include <string>

namespace {

namespace beacon = prowsetk::plugins::beacon;

const char* const kCapabilities[] = {
    "beacon-oracle",
    "flash-lifecycle",
    "page-dom",
    "network-info",
    "stylesheet",
    "tracepoint",
};

const ProwseTkPluginInfo kInfo = {
    "beacon",
    "0.1.0",
    "2",
    "Firefox Native Messaging oracle: Flash requests bridged through beacond with explicit user consent",
    PROWSETK_PLUGIN_NATIVE,
    "beacon",
    "prowsetk-plugin",
    kCapabilities,
    6,
};

beacon::BeaconOptions g_options;

bool truthy(const char* value) {
    return value != nullptr &&
           (std::strcmp(value, "1") == 0 || std::strcmp(value, "true") == 0 ||
            std::strcmp(value, "yes") == 0 || std::strcmp(value, "on") == 0);
}

int plugin_initialize(ProwseTkHost* host) {
    if (host == nullptr || host->api == nullptr) return PROWSETK_STATUS_ERROR;
    if (host->api->abi_version != PROWSETK_PLUGIN_ABI_VERSION) {
        return PROWSETK_STATUS_ERROR;
    }
    if (host->api->log != nullptr) {
        host->api->log(host->api->user_data, PROWSETK_LOG_INFO,
                       "beacon plugin initialized");
    }
    return PROWSETK_STATUS_OK;
}

void plugin_shutdown(ProwseTkHost* host) {
    if (host != nullptr && host->api != nullptr && host->api->log != nullptr) {
        host->api->log(host->api->user_data, PROWSETK_LOG_INFO,
                       "beacon plugin shutdown");
    }
}

const ProwseTkPluginInfo* plugin_info() { return &kInfo; }

int plugin_configure(ProwseTkHost*, const ProwseTkConfigEntry* entries,
                     std::size_t entry_count) {
    if (entries == nullptr && entry_count != 0) {
        return PROWSETK_STATUS_INVALID_ARGUMENT;
    }
    for (std::size_t i = 0; i < entry_count; ++i) {
        const ProwseTkConfigEntry& entry = entries[i];
        if (entry.key == nullptr) continue;
        const std::string key = entry.key;
        const std::string value = entry.value != nullptr ? entry.value : "";
        if (key == "beacon.socket_path" && !value.empty()) {
            g_options.socket_path = value;
        } else if (key == "beacon.timeout_ms" && !value.empty()) {
            try {
                g_options.timeout_ms =
                    static_cast<std::uint32_t>(std::stoul(value));
            } catch (...) {
                return PROWSETK_STATUS_INVALID_ARGUMENT;
            }
        } else if (key == "beacon.max_flashes" && !value.empty()) {
            try {
                g_options.max_flashes =
                    static_cast<std::uint32_t>(std::stoul(value));
            } catch (...) {
                return PROWSETK_STATUS_INVALID_ARGUMENT;
            }
        } else if (key == "beacon.require_consent") {
            g_options.require_user_consent = truthy(entry.value);
        } else if (key == "beacon.redact_secrets") {
            g_options.redact_secrets = truthy(entry.value);
        } else if (key == "beacon.auth_token") {
            // Secret-bearing: stored but never logged or emitted.
            g_options.auth_token = value;
        }
    }
    return PROWSETK_STATUS_OK;
}

const ProwseTkPlugin kPlugin = {plugin_initialize, plugin_shutdown,
                                plugin_info,       plugin_configure,
                                nullptr,           nullptr,
                                nullptr};

}  // namespace

extern "C" {
PROWSETK_PLUGIN_EXPORT const ProwseTkPlugin* prowsetk_plugin_entry(void) {
    return &kPlugin;
}
}
