#include "prowsetk/plugin_registry.hpp"

#include <mutex>
#include <string>

#include "prowsetk/error.hpp"

#include <algorithm>
#include <cctype>

#if defined(__unix__) || defined(__APPLE__)
#include <dlfcn.h>
#endif

namespace prowsetk {
namespace {

struct LoadedPlugin {
    PluginDescriptor descriptor;
    void* handle = nullptr;
    const ProwseTkPlugin* plugin = nullptr;
    WasmSandboxConfig wasm_config;
};

std::string abi_string() { return std::to_string(PROWSETK_PLUGIN_ABI_VERSION); }

}  // namespace

struct PluginRegistry::Impl {
    std::mutex mutex;
    std::vector<LoadedPlugin> plugins;
    ProwseTkHostApi host_api{};
    ProwseTkHost host{};
    void (*log_sink)(void*, int, const char*) = nullptr;
    void* log_user_data = nullptr;
    EventDispatcher* event_dispatcher = nullptr;

    void emit_plugin_event(EventType type, const PluginDescriptor& descriptor) {
        if (event_dispatcher == nullptr) {
            return;
        }
        Event event;
        event.type = type;
        event.name = descriptor.name;
        event.url = descriptor.path;
        event.attributes["version"] = descriptor.version;
        event_dispatcher->emit(event);
    }

    static void log_bridge(void* user_data, int level, const char* message) {
        auto* impl = static_cast<Impl*>(user_data);
        if (impl == nullptr) {
            return;
        }
        if (impl->log_sink != nullptr) {
            impl->log_sink(impl->log_user_data, level, message);
        }
    }

    ProwseTkHost* host_handle() {
        host_api.abi_version = PROWSETK_PLUGIN_ABI_VERSION;
        host_api.user_data = this;
        host_api.log = &Impl::log_bridge;
        host.api = &host_api;
        return &host;
    }
};

PluginRegistry::PluginRegistry() : impl_(std::make_unique<Impl>()) {}

PluginRegistry::~PluginRegistry() { shutdown_all(); }

const PluginDescriptor& PluginRegistry::load_native(
    const std::filesystem::path& path) {
#if defined(__unix__) || defined(__APPLE__)
    void* handle = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
        const char* error = ::dlerror();
        throw Error(ErrorCode::PluginError,
                    "failed to load plugin " + path.string() + ": " +
                        (error != nullptr ? error : "unknown error"));
    }
    auto* entry = reinterpret_cast<ProwseTkPluginEntry>(
        ::dlsym(handle, PROWSETK_PLUGIN_ENTRY_SYMBOL));
    if (entry == nullptr) {
        ::dlclose(handle);
        throw Error(ErrorCode::PluginError,
                    "plugin missing entry symbol " PROWSETK_PLUGIN_ENTRY_SYMBOL);
    }
    const ProwseTkPlugin* plugin = entry();
    if (plugin == nullptr || plugin->info == nullptr) {
        ::dlclose(handle);
        throw Error(ErrorCode::PluginError, "plugin entry returned no info");
    }
    const ProwseTkPluginInfo* info = plugin->info();
    if (info == nullptr || info->name == nullptr) {
        ::dlclose(handle);
        throw Error(ErrorCode::PluginError, "plugin info is incomplete");
    }
    if (info->abi_version == nullptr || abi_string() != info->abi_version) {
        ::dlclose(handle);
        throw Error(ErrorCode::PluginError,
                    "plugin ABI mismatch: expected " + abi_string() + ", got " +
                        (info->abi_version != nullptr ? info->abi_version
                                                      : "<null>"));
    }

    LoadedPlugin loaded;
    loaded.handle = handle;
    loaded.plugin = plugin;
    loaded.descriptor.name = info->name;
    loaded.descriptor.version = info->version != nullptr ? info->version : "";
    loaded.descriptor.abi_version = info->abi_version;
    loaded.descriptor.description =
        info->description != nullptr ? info->description : "";
    loaded.descriptor.path = path.string();
    loaded.descriptor.type = info->type;
    for (std::size_t i = 0; i < info->capability_count; ++i) {
        if (info->capabilities != nullptr && info->capabilities[i] != nullptr) {
            loaded.descriptor.capabilities.emplace_back(info->capabilities[i]);
        }
    }

    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->plugins.push_back(std::move(loaded));
    return impl_->plugins.back().descriptor;
#else
    (void)path;
    throw Error(ErrorCode::Unsupported,
                "native plugin loading is not supported on this platform");
#endif
}

const PluginDescriptor& PluginRegistry::load_lua(
    const std::filesystem::path& path) {
    LoadedPlugin loaded;
    loaded.descriptor.name = path.stem().string();
    loaded.descriptor.path = path.string();
    loaded.descriptor.type = PROWSETK_PLUGIN_LUA;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->plugins.push_back(std::move(loaded));
    return impl_->plugins.back().descriptor;
}

const PluginDescriptor& PluginRegistry::load_wasm(
    const std::filesystem::path& path, const WasmSandboxConfig& config) {
    LoadedPlugin loaded;
    loaded.descriptor.name = path.stem().string();
    loaded.descriptor.path = path.string();
    loaded.descriptor.type = PROWSETK_PLUGIN_WASM;
    loaded.wasm_config = config;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->plugins.push_back(std::move(loaded));
    return impl_->plugins.back().descriptor;
}

std::size_t PluginRegistry::initialize_all() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    std::size_t count = 0;
    for (auto& loaded : impl_->plugins) {
        if (loaded.descriptor.initialized || loaded.plugin == nullptr) {
            continue;
        }
        ProwseTkHost* host = impl_->host_handle();
        int rc = 0;
        try {
            rc = loaded.plugin->initialize != nullptr
                     ? loaded.plugin->initialize(host)
                     : 0;
        } catch (...) {
            rc = -1;
        }
        if (rc != 0) {
            throw Error(ErrorCode::PluginError,
                        "plugin failed to initialize: " +
                            loaded.descriptor.name);
        }
        loaded.descriptor.initialized = true;
        impl_->emit_plugin_event(EventType::PluginInit, loaded.descriptor);
        ++count;
    }
    return count;
}

void PluginRegistry::shutdown_all() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (auto& loaded : impl_->plugins) {
        impl_->emit_plugin_event(EventType::PluginShutdown, loaded.descriptor);
        if (loaded.plugin != nullptr && loaded.plugin->shutdown != nullptr &&
            loaded.descriptor.initialized) {
            try {
                loaded.plugin->shutdown(impl_->host_handle());
            } catch (...) {
                // Shutdown must not throw across the C ABI boundary.
            }
        }
        loaded.descriptor.initialized = false;
#if defined(__unix__) || defined(__APPLE__)
        if (loaded.handle != nullptr) {
            ::dlclose(loaded.handle);
            loaded.handle = nullptr;
        }
#endif
    }
    impl_->plugins.clear();
}

std::vector<PluginDescriptor> PluginRegistry::plugins() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    std::vector<PluginDescriptor> result;
    result.reserve(impl_->plugins.size());
    for (const auto& loaded : impl_->plugins) {
        result.push_back(loaded.descriptor);
    }
    return result;
}

const PluginDescriptor* PluginRegistry::find(std::string_view name) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (const auto& loaded : impl_->plugins) {
        if (loaded.descriptor.name == name) {
            return &loaded.descriptor;
        }
    }
    return nullptr;
}

std::size_t PluginRegistry::discover(const std::filesystem::path& directory) {
    std::vector<std::string> warnings;
    return discover(directory, warnings);
}

std::size_t PluginRegistry::discover(const std::filesystem::path& directory,
                                     std::vector<std::string>& warnings) {
    warnings.clear();
    if (!std::filesystem::exists(directory) ||
        !std::filesystem::is_directory(directory)) {
        warnings.push_back("plugin directory does not exist: " + directory.string());
        return 0;
    }
    std::size_t loaded = 0;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (!entry.is_regular_file()) continue;
        const auto path = entry.path();
        const auto ext = path.extension().string();
        std::string lower = ext;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
        try {
            if (lower == ".so" || lower == ".dylib" || lower == ".dll") {
                load_native(path);
                ++loaded;
            } else if (lower == ".lua") {
                load_lua(path);
                ++loaded;
            } else if (lower == ".wasm") {
                load_wasm(path, WasmSandboxConfig{});
                ++loaded;
            }
        } catch (const std::exception& ex) {
            warnings.push_back(path.string() + ": " + ex.what());
        } catch (...) {
            warnings.push_back(path.string() + ": unknown error during discovery");
        }
    }
    return loaded;
}

bool PluginRegistry::has_capability(std::string_view capability) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (const auto& loaded : impl_->plugins) {
        for (const auto& cap : loaded.descriptor.capabilities) {
            if (cap == capability) return true;
        }
    }
    return false;
}

void PluginRegistry::set_event_dispatcher(EventDispatcher* dispatcher) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->event_dispatcher = dispatcher;
}

void PluginRegistry::set_log_sink(
    void (*sink)(void* user_data, int level, const char* message),
    void* user_data) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->log_sink = sink;
    impl_->log_user_data = user_data;
}

}  // namespace prowsetk
