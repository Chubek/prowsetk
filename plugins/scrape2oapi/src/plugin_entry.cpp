#include "prowsetk/ProwseTk-Plugin.h"
#include "prowsetk/plugins/scrape2oapi.hpp"

#include <cstring>
#include <string>

namespace {
const char* const kCapabilities[] = {
    "scrape2oapi",
    "endpoint-discovery",
    "openapi-yaml",
    "chain-resolution",
};

const ProwseTkPluginInfo kInfo = {
    "scrape2oapi",
    "0.1.0",
    "1",
    "Scrape internal API endpoints from a webpage and dump OpenAPI YAML, optionally resolving redirect/API chains",
    PROWSETK_PLUGIN_NATIVE,
    kCapabilities,
    4,
};

int plugin_initialize(ProwseTkHost* host) {
    if (host == nullptr || host->api == nullptr) return 1;
    if (host->api->abi_version != PROWSETK_PLUGIN_ABI_VERSION) return 2;
    if (host->api->log != nullptr) {
        host->api->log(host->api->user_data, PROWSETK_LOG_INFO,
                       "scrape2oapi plugin initialized");
    }
    return 0;
}

void plugin_shutdown(ProwseTkHost* host) {
    if (host != nullptr && host->api != nullptr && host->api->log != nullptr) {
        host->api->log(host->api->user_data, PROWSETK_LOG_INFO,
                       "scrape2oapi plugin shutdown");
    }
}

const ProwseTkPluginInfo* plugin_info() { return &kInfo; }

const ProwseTkPlugin kPlugin = {plugin_initialize, plugin_shutdown, plugin_info};

}  // namespace

extern "C" {
PROWSETK_PLUGIN_EXPORT const ProwseTkPlugin* prowsetk_plugin_entry(void) {
    return &kPlugin;
}
}
