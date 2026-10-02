#include "prowsetk/ProwseTk-Plugin.h"

namespace {
const char* const capabilities[] = {
    "ai-oracle", "openai-responses", "captcha-advice", "crawl-advice", "embedded-image-input", "structured-advice"};
const ProwseTkPluginInfo info = {
    "ai-oracle", "0.1.0", "2", "Bounded OpenAI oracle for plugins and automation drivers",
    PROWSETK_PLUGIN_NATIVE, "ai_oracle", nullptr, capabilities, 6};

int initialize(ProwseTkHost* host) noexcept {
    return host && host->api && host->api->abi_version == PROWSETK_PLUGIN_ABI_VERSION
        ? PROWSETK_STATUS_OK : PROWSETK_STATUS_INVALID_ARGUMENT;
}
const ProwseTkPluginInfo* plugin_info() noexcept { return &info; }
// Inquiries are explicit service calls. Loading the plugin is network-free.
const ProwseTkPlugin plugin{initialize, nullptr, plugin_info, nullptr, nullptr, nullptr, nullptr};
}  // namespace

extern "C" PROWSETK_PLUGIN_EXPORT const ProwseTkPlugin* prowsetk_plugin_entry(void) {
    return &plugin;
}
