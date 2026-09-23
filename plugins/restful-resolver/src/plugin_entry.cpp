#include "prowsetk/ProwseTk-Plugin.h"

#include <cstddef>

namespace {

const char* const kCapabilities[] = {
    "restful-resolution",
    "endpoint-discovery",
    "openapi-yaml",
    "post-discovery",
};

const ProwseTkPluginInfo kInfo = {
    "restful-resolver",
    "0.1.0",
    "2",
    "Resolve scraped endpoints iteratively until a full RESTful API surface (GET+POST) is discovered",
    PROWSETK_PLUGIN_NATIVE,
    "restful_resolver",
    "prowsetk-plugin",
    kCapabilities,
    4,
};

int plugin_initialize(ProwseTkHost* host) {
    if (host == nullptr || host->api == nullptr) return 1;
    if (host->api->abi_version != PROWSETK_PLUGIN_ABI_VERSION) return 2;
    if (host->api->log != nullptr) {
        host->api->log(host->api->user_data, PROWSETK_LOG_INFO,
                       "restful-resolver plugin initialized");
    }
    return 0;
}

void plugin_shutdown(ProwseTkHost* host) {
    if (host != nullptr && host->api != nullptr && host->api->log != nullptr) {
        host->api->log(host->api->user_data, PROWSETK_LOG_INFO,
                       "restful-resolver plugin shutdown");
    }
}

const ProwseTkPluginInfo* plugin_info() { return &kInfo; }

const ProwseTkPlugin kPlugin = {plugin_initialize, plugin_shutdown, plugin_info,
                                nullptr,           nullptr,         nullptr,
                                nullptr};

}  // namespace

extern "C" {
PROWSETK_PLUGIN_EXPORT const ProwseTkPlugin* prowsetk_plugin_entry(void) {
    return &kPlugin;
}
}
