#include <prowsetk/ProwseTk-Plugin.h>
namespace {
const char* const capabilities[] = {"display-list-browser", "explicit-session-control", "host-mediated-interaction"};
const ProwseTkPluginInfo info = {"complex-gui", "0.1.0", "2",
    "Flatworm display-list desktop browser; construct its C++ Viewer explicitly",
    PROWSETK_PLUGIN_NATIVE, nullptr, nullptr, capabilities, 3};
int initialize(ProwseTkHost* host) noexcept {
    return host && host->api && host->api->abi_version == PROWSETK_PLUGIN_ABI_VERSION
        ? PROWSETK_STATUS_OK : PROWSETK_STATUS_INVALID_ARGUMENT;
}
void shutdown(ProwseTkHost*) noexcept {}
const ProwseTkPluginInfo* get_info() noexcept { return &info; }
const ProwseTkPlugin plugin = {initialize, shutdown, get_info, nullptr, nullptr, nullptr, nullptr};
}
extern "C" PROWSETK_PLUGIN_EXPORT const ProwseTkPlugin* prowsetk_plugin_entry(void) { return &plugin; }
