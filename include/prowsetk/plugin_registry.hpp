#ifndef PROWSETK_PLUGIN_REGISTRY_HPP
#define PROWSETK_PLUGIN_REGISTRY_HPP

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "prowsetk/ProwseTk-Plugin.h"
#include "prowsetk/wasm_runtime.hpp"

namespace prowsetk {

struct PluginDescriptor {
    std::string name;
    std::string version;
    std::string abi_version;
    std::string description;
    std::string path;
    ProwseTkPluginType type = PROWSETK_PLUGIN_NATIVE;
    std::vector<std::string> capabilities;
    bool initialized = false;
};

// Loads and tracks native, Lua, and WASM plugins. The C++ registry mirrors the
// C ABI in ProwseTk-Plugin.h and may evolve independently of it.
class PluginRegistry {
public:
    PluginRegistry();
    ~PluginRegistry();

    PluginRegistry(const PluginRegistry&) = delete;
    PluginRegistry& operator=(const PluginRegistry&) = delete;

    // Loads a native shared object, validates its ABI version, and records its
    // descriptor. Does not call initialize().
    const PluginDescriptor& load_native(const std::filesystem::path& path);

    // Records a Lua plugin module path. Lua plugins are executed by the Lua
    // runtime; the registry only tracks metadata.
    const PluginDescriptor& load_lua(const std::filesystem::path& path);

    // Records a WASM plugin and its sandbox configuration.
    const PluginDescriptor& load_wasm(const std::filesystem::path& path,
                                      const WasmSandboxConfig& config = {});

    // Initializes every loaded plugin that has not yet been initialized.
    // Returns the number of plugins initialized. On failure the offending
    // plugin is marked and an Error is thrown.
    std::size_t initialize_all();

    void shutdown_all();

    std::vector<PluginDescriptor> plugins() const;
    const PluginDescriptor* find(std::string_view name) const;

    void set_log_sink(void (*sink)(void* user_data, int level, const char* message),
                      void* user_data);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace prowsetk

#endif  // PROWSETK_PLUGIN_REGISTRY_HPP
