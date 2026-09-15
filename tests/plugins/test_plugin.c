#include "prowsetk/ProwseTk-Plugin.h"

#include <string.h>

static const char* const kCapabilities[] = {"document-extractor",
                                            "openapi-yaml"};

static const ProwseTkPluginInfo kInfo = {
    "test-plugin",
    "1.0.0",
    "1",
    "Plugin used by the ProwseTk test suite",
    PROWSETK_PLUGIN_NATIVE,
    kCapabilities,
    2};

static int g_initialized = 0;

static int plugin_initialize(ProwseTkHost* host) {
    if (host == NULL || host->api == NULL) {
        return 1;
    }
    if (host->api->abi_version != PROWSETK_PLUGIN_ABI_VERSION) {
        return 2;
    }
    if (host->api->log != NULL) {
        host->api->log(host->api->user_data, PROWSETK_LOG_INFO,
                       "test-plugin initialized");
    }
    g_initialized = 1;
    return 0;
}

static void plugin_shutdown(ProwseTkHost* host) {
    (void)host;
    g_initialized = 0;
}

static const ProwseTkPluginInfo* plugin_info(void) { return &kInfo; }

static const ProwseTkPlugin kPlugin = {plugin_initialize, plugin_shutdown,
                                       plugin_info};

PROWSETK_PLUGIN_EXPORT const ProwseTkPlugin* prowsetk_plugin_entry(void) {
    return &kPlugin;
}

PROWSETK_PLUGIN_EXPORT int prowsetk_test_plugin_is_initialized(void) {
    return g_initialized;
}
