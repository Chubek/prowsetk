#include "prowsetk/ProwseTk-Plugin.h"
#include "prowsetk/plugins/schema_grabber.hpp"

#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <string_view>

namespace {
namespace grabber = prowsetk::plugins::schema_grabber;

struct Config {
    grabber::SchemaGrabberOptions options;
    std::filesystem::path openapi_output;
    std::filesystem::path postman_output;
};

std::mutex mutex;
std::map<ProwseTkHost*, Config> configs;
const char* const capabilities[] = {"schema-inference", "endpoint-discovery",
                                    "openapi-yaml", "postman-json",
                                    "url-parameters"};
const ProwseTkPluginInfo info = {
    "schema-grabber", "0.1.0", "2",
    "Reverse-engineer request/response schemas and URL parameters for scraped endpoints",
    PROWSETK_PLUGIN_NATIVE, "schema_grabber", "prowsetk-plugin",
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
            if (key == "schema-grabber.output_openapi") {
                next.openapi_output = value;
            } else if (key == "schema-grabber.output_postman") {
                next.postman_output = value;
            } else if (key == "schema-grabber.collection_name") {
                next.options.collection_name = value;
            } else if (key == "schema-grabber.include_provenance") {
                if (!parse_bool(value, parsed)) return PROWSETK_STATUS_INVALID_ARGUMENT;
                next.options.include_provenance = parsed;
            } else if (key == "schema-grabber.probe_get_responses") {
                if (!parse_bool(value, parsed)) return PROWSETK_STATUS_INVALID_ARGUMENT;
                next.options.probe_get_responses = parsed;
            } else if (key.starts_with("schema-grabber.")) {
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
        const auto result =
            grabber::grab_from_document(*document, config.options);
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
