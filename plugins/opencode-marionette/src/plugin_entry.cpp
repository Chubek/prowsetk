#include "prowsetk/ProwseTk-Plugin.h"
namespace {
const char* const capabilities[] = {"opencode-marionette", "json-decisions", "explicit-session-control", "endpoint-schema-discovery"};
const ProwseTkPluginInfo info = {"opencode-marionette", "0.1.0", "2",
    "Explicit OpenCode page controller with JSON action policy and endpoint schemas",
    PROWSETK_PLUGIN_NATIVE, nullptr, nullptr, capabilities, 4};
int initialize(ProwseTkHost* host) noexcept {
    return host && host->api && host->api->abi_version == PROWSETK_PLUGIN_ABI_VERSION
        ? PROWSETK_STATUS_OK : PROWSETK_STATUS_INVALID_ARGUMENT;
}
void shutdown(ProwseTkHost*) noexcept {}
const ProwseTkPluginInfo* get_info() noexcept { return &info; }
const ProwseTkPlugin plugin = {initialize, shutdown, get_info, nullptr, nullptr, nullptr, nullptr};
}
extern "C" PROWSETK_PLUGIN_EXPORT const ProwseTkPlugin* prowsetk_plugin_entry(void) { return &plugin; }
