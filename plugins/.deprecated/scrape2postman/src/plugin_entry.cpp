#include "prowsetk/ProwseTk-Plugin.h"
#include "prowsetk/plugins/scrape2postman.hpp"

#include <map>
#include <mutex>

namespace {
namespace scrape = prowsetk::plugins::scrape2postman;
struct Config {
    scrape::Scrape2PostmanOptions options;
    std::filesystem::path output;
};
std::mutex mutex;
std::map<ProwseTkHost*, Config> configs;
const char* const capabilities[] = {"scrape2postman", "endpoint-discovery", "postman-json"};
const ProwseTkPluginInfo info = {
    "scrape2postman", "0.1.0", "2",
    "Export heuristic endpoint discoveries as Postman Collection v2.1 JSON",
    PROWSETK_PLUGIN_NATIVE, "scrape2postman", nullptr, capabilities, 3
};

int initialize(ProwseTkHost* host) noexcept {
    if (!host || !host->api || host->api->abi_version != PROWSETK_PLUGIN_ABI_VERSION)
        return PROWSETK_STATUS_INVALID_ARGUMENT;
    try {
        std::lock_guard lock(mutex);
        configs.try_emplace(host);
        return PROWSETK_STATUS_OK;
    } catch (...) { return PROWSETK_STATUS_ERROR; }
}
void shutdown(ProwseTkHost* host) noexcept {
    try { std::lock_guard lock(mutex); configs.erase(host); } catch (...) {}
}
const ProwseTkPluginInfo* plugin_info() noexcept { return &info; }

int configure(ProwseTkHost* host, const ProwseTkConfigEntry* entries, size_t count) noexcept {
    if (!host || (count && !entries)) return PROWSETK_STATUS_INVALID_ARGUMENT;
    try {
        std::lock_guard lock(mutex);
        auto it = configs.find(host);
        if (it == configs.end()) return PROWSETK_STATUS_INVALID_ARGUMENT;
        auto next = it->second;
        for (size_t i = 0; i < count; ++i) {
            if (!entries[i].key || !entries[i].value) return PROWSETK_STATUS_INVALID_ARGUMENT;
            const std::string_view key(entries[i].key), value(entries[i].value);
            if (key == "scrape2postman.output") next.output = value;
            else if (key == "scrape2postman.collection_name") next.options.collection_name = value;
            else if (key == "scrape2postman.include_provenance") {
                if (value != "true" && value != "false") return PROWSETK_STATUS_INVALID_ARGUMENT;
                next.options.include_provenance = value == "true";
            } else if (key.starts_with("scrape2postman.")) return PROWSETK_STATUS_INVALID_ARGUMENT;
        }
        it->second = std::move(next);
        return PROWSETK_STATUS_OK;
    } catch (...) { return PROWSETK_STATUS_ERROR; }
}

int on_document(ProwseTkHost* host, const ProwseTkDocumentSnapshot* snapshot) noexcept {
    if (!host || !snapshot || !snapshot->html) return PROWSETK_STATUS_INVALID_ARGUMENT;
    try {
        Config config;
        {
            std::lock_guard lock(mutex);
            const auto it = configs.find(host);
            if (it == configs.end()) return PROWSETK_STATUS_INVALID_ARGUMENT;
            config = it->second;
        }
        // Explicit output configuration opts into writing, once per document.
        if (config.output.empty()) return PROWSETK_STATUS_OK;
        const auto document = prowsetk::parse_html(snapshot->html, snapshot->url ? snapshot->url : "");
        const auto result = scrape::scrape_from_document(*document, config.options);
        result.write_postman_json(config.output);
        return PROWSETK_STATUS_OK;
    } catch (...) { return PROWSETK_STATUS_ERROR; }
}
const ProwseTkPlugin plugin = {initialize, shutdown, plugin_info, configure,
                               nullptr, nullptr, on_document};
}  // namespace

extern "C" PROWSETK_PLUGIN_EXPORT const ProwseTkPlugin* prowsetk_plugin_entry(void) {
    return &plugin;
}
