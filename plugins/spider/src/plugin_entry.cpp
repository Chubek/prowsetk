#include <prowsetk/plugins/spider.hpp>
#include <prowsetk/ProwseTk-Plugin.h>
#include <map>
#include <mutex>

namespace {
namespace spider = prowsetk::plugins::spider;
std::mutex mutex;
std::map<ProwseTkHost*, std::unique_ptr<spider::Cache>> caches;
constexpr const char* capabilities[] = {"persistent-crawlers", "spider-control", "lmdb-cache", "lua-drivers"};
const ProwseTkPluginInfo info = {"spider", "0.1.0", "2",
    "Persistent headless crawler workers controlled by ptkspiderd/ptkspiderctl",
    PROWSETK_PLUGIN_NATIVE, "lspider", nullptr, capabilities, 4};
int initialize(ProwseTkHost* host) {
    return host && host->api && host->api->abi_version == PROWSETK_PLUGIN_ABI_VERSION ?
        PROWSETK_STATUS_OK : PROWSETK_STATUS_INVALID_ARGUMENT;
}
void shutdown(ProwseTkHost* host) {
    try { std::lock_guard lock(mutex); caches.erase(host); } catch (...) {}
}
const ProwseTkPluginInfo* metadata() { return &info; }
int configure(ProwseTkHost* host, const ProwseTkConfigEntry* entries, std::size_t count) {
    if (!host || (!entries && count)) return PROWSETK_STATUS_INVALID_ARGUMENT;
    try {
        std::lock_guard lock(mutex);
        for (std::size_t i = 0; i < count; ++i) {
            if (!entries[i].key || !entries[i].value) return PROWSETK_STATUS_INVALID_ARGUMENT;
            const std::string_view key(entries[i].key);
            if (key != "cache_directory" && key != "spider.cache_directory") return PROWSETK_STATUS_INVALID_ARGUMENT;
            caches.erase(host);
            caches[host] = std::make_unique<spider::Cache>(entries[i].value);
        }
        return PROWSETK_STATUS_OK;
    } catch (...) { return PROWSETK_STATUS_ERROR; }
}
int document(ProwseTkHost* host, const ProwseTkDocumentSnapshot* snapshot) {
    if (!host || !snapshot || !snapshot->url) return PROWSETK_STATUS_INVALID_ARGUMENT;
    try {
        std::lock_guard lock(mutex);
        const auto it = caches.find(host);
        if (it != caches.end()) it->second->write({{"record/latest-url", prowsetk::Redactor{}.redact_url(snapshot->url)}});
        return PROWSETK_STATUS_OK;
    } catch (...) { return PROWSETK_STATUS_ERROR; }
}
const ProwseTkPlugin plugin = {initialize, shutdown, metadata, configure, nullptr, nullptr, document};
}
extern "C" PROWSETK_PLUGIN_EXPORT const ProwseTkPlugin* prowsetk_plugin_entry(void) { return &plugin; }
