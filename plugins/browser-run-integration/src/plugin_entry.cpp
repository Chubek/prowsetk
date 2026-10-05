#include "prowsetk/ProwseTk-Plugin.h"
namespace {
const char* const capabilities[] = {"browser-run-cdp", "browser-run-content", "remote-html-import"};
const ProwseTkPluginInfo info{"browser-run-integration", "0.1.0", "2", "Explicit Cloudflare Browser Run client",
    PROWSETK_PLUGIN_NATIVE, nullptr, nullptr, capabilities, 3};
int initialize(ProwseTkHost* host) noexcept {
    return host && host->api && host->api->abi_version == PROWSETK_PLUGIN_ABI_VERSION ? PROWSETK_STATUS_OK : PROWSETK_STATUS_INVALID_ARGUMENT;
}
const ProwseTkPluginInfo* metadata() noexcept { return &info; }
const ProwseTkPlugin plugin{initialize, nullptr, metadata, nullptr, nullptr, nullptr, nullptr};
}
extern "C" PROWSETK_PLUGIN_EXPORT const ProwseTkPlugin* prowsetk_plugin_entry(void) { return &plugin; }
