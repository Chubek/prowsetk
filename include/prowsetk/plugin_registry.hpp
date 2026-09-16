#ifndef PROWSETK_PLUGIN_REGISTRY_HPP
#define PROWSETK_PLUGIN_REGISTRY_HPP

#include <filesystem>
#include <memory>
#include <utility>
#include <string>
#include <string_view>
#include <vector>

#include "prowsetk/ProwseTk-Plugin.h"
#include "prowsetk/document.hpp"
#include "prowsetk/event.hpp"
#include "prowsetk/network_client.hpp"
#include "prowsetk/wasm_runtime.hpp"

namespace prowsetk {

struct PluginDescriptor {
    std::string name;
    std::string version;
    std::string abi_version;
    std::string description;
    std::string path;
    std::string lua_module;
    std::string wasm_world;
    ProwseTkPluginType type = PROWSETK_PLUGIN_NATIVE;
    std::vector<std::string> capabilities;
    bool initialized = false;
};

// Loads and tracks native, Lua, and WASM plugins. The C++ registry mirrors the
// C ABI in ProwseTk-Plugin.h and may evolve independently of it. The registry
// is the single owner of loaded plugin handles: it validates the ABI version
// before recording a descriptor, holds the dlopen handle until shutdown_all or
// destruction, translates C++ exceptions at the C ABI boundary into error
// codes, and never leaks C++ types across the plugin interface.
class PluginRegistry {
public:
    PluginRegistry();
    ~PluginRegistry();

    PluginRegistry(const PluginRegistry&) = delete;
    PluginRegistry& operator=(const PluginRegistry&) = delete;

    // Loads a native shared object, validates its ABI version, and records its
    // descriptor. Does not call initialize(). Throws Error(PluginError) when the
    // file cannot be opened, the entry symbol is missing, or the ABI version
    // mismatches. The dlopen handle is retained until shutdown_all.
    const PluginDescriptor& load_native(const std::filesystem::path& path);

    // Records a Lua plugin module path. Lua plugins are executed by the Lua
    // runtime; the registry only tracks metadata and leaves execution to
    // LuaRuntime. Paths are stored verbatim so callers may pass absolute or
    // project-relative locations.
    const PluginDescriptor& load_lua(const std::filesystem::path& path);

    // Records a WASM plugin and its sandbox configuration. When the build has
    // no WASM runtime linked, the descriptor is still tracked but instantiation
    // will later report WasmError. Configuration is per-plugin (AGENTS.md 7,
    // README "Plugin configuration").
    const PluginDescriptor& load_wasm(const std::filesystem::path& path,
                                      const WasmSandboxConfig& config = {});

    // Scans `directory` for shared objects (".so", ".dylib", ".dll") and Lua
    // modules (".lua") and loads each via load_native/load_lua. WASM modules
    // (".wasm") are loaded with a default sandbox unless a config map is
    // supplied. Returns the number of plugins discovered. Errors for individual
    // files are collected and reported as warnings rather than aborting the
    // entire scan.
    std::size_t discover(const std::filesystem::path& directory);
    std::size_t discover(const std::filesystem::path& directory,
                         std::vector<std::string>& warnings);

    // Initializes every loaded plugin that has not yet been initialized.
    // Returns the number of plugins initialized. On failure the offending
    // plugin is marked and an Error is thrown. Exceptions never cross the C
    // ABI: plugin initialize() returns an int; C++ exceptions are caught and
    // translated to a nonzero code at the boundary.
    std::size_t initialize_all();

    // Passes declarative configuration entries to initialized native plugins
    // that expose configure(). Non-native descriptors are ignored.
    std::size_t configure_all(
        const std::vector<std::pair<std::string, std::string>>& entries = {});

    // Dispatches host-mediated request/response/document hooks to initialized
    // native plugins. before_request may replace the request in-place or reject
    // it with Error(SecurityViolation). after_response may reject the response.
    // Document hooks receive snapshots only; plugins never receive C++ DOM
    // objects across the C ABI.
    void dispatch_before_request(HttpRequest& request);
    void dispatch_after_response(const HttpRequest& request,
                                 const HttpResponse& response);
    void dispatch_document(const Document& document);

    void shutdown_all();

    // Returns a snapshot of registered descriptors. The caller receives copies
    // so the registry remains the sole owner of handles and plugin pointers.
    std::vector<PluginDescriptor> plugins() const;
    const PluginDescriptor* find(std::string_view name) const;
    bool has_capability(std::string_view capability) const;

    // Opt-in event channel. When set, initialize_all and shutdown_all emit
    // PluginInit and PluginShutdown events for every plugin. The dispatcher is
    // not owned by the registry.
    void set_event_dispatcher(EventDispatcher* dispatcher);

    // Installs an optional host log sink that plugins may call through
    // ProwseTkHostApi::log. The sink is invoked synchronously from
    // Impl::log_bridge; it must not call back into the registry.
    void set_log_sink(void (*sink)(void* user_data, int level, const char* message),
                      void* user_data);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace prowsetk

#endif  // PROWSETK_PLUGIN_REGISTRY_HPP
