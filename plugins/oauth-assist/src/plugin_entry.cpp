#include "prowsetk/ProwseTk-Plugin.h"
namespace {
const char* const capabilities[] = {"oauth2-pkce", "oauth-token-cache"};
const ProwseTkPluginInfo info{"oauth-assist", "0.1.0", "2", "Explicit host-mediated OAuth assistance",
    PROWSETK_PLUGIN_NATIVE, nullptr, nullptr, capabilities, 2};
int initialize(ProwseTkHost* host) noexcept {
    return host && host->api && host->api->abi_version == PROWSETK_PLUGIN_ABI_VERSION ? PROWSETK_STATUS_OK : PROWSETK_STATUS_INVALID_ARGUMENT;
}
const ProwseTkPluginInfo* metadata() noexcept { return &info; }
const ProwseTkPlugin plugin{initialize, nullptr, metadata, nullptr, nullptr, nullptr, nullptr};
}
extern "C" PROWSETK_PLUGIN_EXPORT const ProwseTkPlugin* prowsetk_plugin_entry(void) { return &plugin; }
