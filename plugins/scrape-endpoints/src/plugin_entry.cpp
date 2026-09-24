#include "prowsetk/ProwseTk-Plugin.h"
#include "prowsetk/plugins/scrape_endpoints.hpp"

#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <string_view>

namespace {
namespace scrape = prowsetk::plugins::scrape_endpoints;

struct Config {
    scrape::ScrapeEndpointsOptions options;
    std::filesystem::path openapi_output;
    std::filesystem::path postman_output;
};

std::mutex mutex;
std::map<ProwseTkHost*, Config> configs;
const char* const capabilities[] = {"scrape-endpoints", "endpoint-discovery",
                                    "openapi-yaml", "postman-json",
                                    "spa-synthetic-interactions"};
const ProwseTkPluginInfo info = {
    "scrape-endpoints", "0.2.0", "2",
    "Scrape modern webpage endpoints and export OpenAPI YAML or Postman JSON",
    PROWSETK_PLUGIN_NATIVE, "scrape-endpoints", "prowsetk-plugin",
    capabilities, 5};

bool parse_bool(std::string_view value, bool& out) {
    if (value == "true") {
        out = true;
        return true;
    }
    if (value == "false") {
        out = false;
        return true;
    }
    return false;
}

int initialize(ProwseTkHost* host) noexcept {
    if (!host || !host->api || host->api->abi_version != PROWSETK_PLUGIN_ABI_VERSION) {
        return PROWSETK_STATUS_INVALID_ARGUMENT;
    }
    try {
        std::lock_guard lock(mutex);
        configs.try_emplace(host);
        return PROWSETK_STATUS_OK;
    } catch (...) {
        return PROWSETK_STATUS_ERROR;
    }
}

void shutdown(ProwseTkHost* host) noexcept {
    try {
        std::lock_guard lock(mutex);
        configs.erase(host);
    } catch (...) {
    }
}

const ProwseTkPluginInfo* plugin_info() noexcept { return &info; }

int configure(ProwseTkHost* host, const ProwseTkConfigEntry* entries,
              size_t count) noexcept {
    if (!host || (count && !entries)) return PROWSETK_STATUS_INVALID_ARGUMENT;
    try {
        std::lock_guard lock(mutex);
        auto it = configs.find(host);
        if (it == configs.end()) return PROWSETK_STATUS_INVALID_ARGUMENT;
        auto next = it->second;
        for (size_t i = 0; i < count; ++i) {
            if (!entries[i].key || !entries[i].value) {
                return PROWSETK_STATUS_INVALID_ARGUMENT;
            }
            const std::string_view key(entries[i].key), value(entries[i].value);
            bool parsed = false;
            if (key == "scrape-endpoints.output_openapi" ||
                key == "scrape2oapi.output") {
                next.openapi_output = value;
            } else if (key == "scrape-endpoints.output_postman" ||
                       key == "scrape2postman.output") {
                next.postman_output = value;
            } else if (key == "scrape-endpoints.collection_name" ||
                       key == "scrape2postman.collection_name") {
                next.options.collection_name = value;
            } else if (key == "scrape-endpoints.include_provenance" ||
                       key == "scrape2postman.include_provenance") {
                if (!parse_bool(value, parsed)) return PROWSETK_STATUS_INVALID_ARGUMENT;
                next.options.include_provenance = parsed;
            } else if (key == "scrape-endpoints.spa_probe") {
                if (!parse_bool(value, parsed)) return PROWSETK_STATUS_INVALID_ARGUMENT;
                next.options.spa_probe = parsed;
            } else if (key == "scrape-endpoints.resolve_chain") {
                if (!parse_bool(value, parsed)) return PROWSETK_STATUS_INVALID_ARGUMENT;
                next.options.resolve_chain = parsed;
            } else if (key.starts_with("scrape-endpoints.") ||
                       key.starts_with("scrape2oapi.") ||
                       key.starts_with("scrape2postman.")) {
                return PROWSETK_STATUS_INVALID_ARGUMENT;
            }
        }
        it->second = std::move(next);
        return PROWSETK_STATUS_OK;
    } catch (...) {
        return PROWSETK_STATUS_ERROR;
    }
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
        if (config.openapi_output.empty() && config.postman_output.empty()) {
            return PROWSETK_STATUS_OK;
        }
        const auto document = prowsetk::parse_html(
            snapshot->html, snapshot->url ? snapshot->url : "");
        const auto result = scrape::scrape_from_document(*document, config.options);
        if (!config.openapi_output.empty()) result.write_openapi_yaml(config.openapi_output);
        if (!config.postman_output.empty()) result.write_postman_json(config.postman_output);
        return PROWSETK_STATUS_OK;
    } catch (...) {
        return PROWSETK_STATUS_ERROR;
    }
}

const ProwseTkPlugin plugin = {initialize, shutdown, plugin_info, configure,
                               nullptr, nullptr, on_document};
}  // namespace

extern "C" PROWSETK_PLUGIN_EXPORT const ProwseTkPlugin* prowsetk_plugin_entry(void) {
    return &plugin;
}
