#include <prowsetk/ProwseTk-Plugin.h>

namespace {
#ifdef PROWSETK_HAVE_BASIC_GUI
const char* const capabilities[] = {"basic-gui", "explicit-session-inspection", "sanitized-dom", "host-mediated-interaction"};
#else
const char* const capabilities[] = {"basic-gui-disabled", "explicit-session-inspection", "sanitized-dom", "host-mediated-interaction"};
#endif
const ProwseTkPluginInfo info = {"basic-gui", "0.1.0", "2",
    "Optional FLTK Flatworm inspector; create the viewer explicitly through its C++ API",
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
