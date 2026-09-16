#include "prowsetk/plugin_registry.hpp"

#include <mutex>
#include <string>

#include "prowsetk/error.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>

#if defined(__unix__) || defined(__APPLE__)
#include <dlfcn.h>
#endif

#include "prowsetk/redaction.hpp"

namespace prowsetk {
namespace {

struct LoadedPlugin {
    PluginDescriptor descriptor;
    void* handle = nullptr;
    const ProwseTkPlugin* plugin = nullptr;
    WasmSandboxConfig wasm_config;
};

std::string abi_string() { return std::to_string(PROWSETK_PLUGIN_ABI_VERSION); }

bool valid_plugin_type(ProwseTkPluginType type) {
    return type == PROWSETK_PLUGIN_NATIVE || type == PROWSETK_PLUGIN_LUA ||
           type == PROWSETK_PLUGIN_WASM;
}

std::string string_or_empty(const char* value) {
    return value != nullptr ? value : "";
}

ProwseTkBytes make_bytes(const std::string& value) {
    return ProwseTkBytes{value.data(), value.size()};
}

std::vector<ProwseTkHeader> make_headers(
    const std::vector<std::pair<std::string, std::string>>& headers) {
    std::vector<ProwseTkHeader> result;
    result.reserve(headers.size());
    for (const auto& [name, value] : headers) {
        result.push_back(ProwseTkHeader{name.c_str(), value.c_str()});
    }
    return result;
}

ProwseTkHttpRequest make_c_request(
    const HttpRequest& request, const std::vector<ProwseTkHeader>& headers) {
    return ProwseTkHttpRequest{request.method.c_str(),
                               request.url.c_str(),
                               headers.data(),
                               headers.size(),
                               make_bytes(request.body),
                               static_cast<std::uint32_t>(request.timeout_ms),
                               request.max_response_bytes};
}

ProwseTkHttpResponse make_c_response(
    const HttpResponse& response, const std::vector<ProwseTkHeader>& headers) {
    const int clamped_status =
        std::clamp(response.status, 0,
                   static_cast<int>(std::numeric_limits<std::uint16_t>::max()));
    return ProwseTkHttpResponse{static_cast<std::uint16_t>(clamped_status),
                                headers.data(),
                                headers.size(),
                                make_bytes(response.body),
                                response.final_url.c_str()};
}

HttpRequest copy_request(const ProwseTkHttpRequest& request) {
    if (request.method == nullptr || request.url == nullptr) {
        throw Error(ErrorCode::PluginError,
                    "plugin replacement request is missing method or URL");
    }
    HttpRequest copied;
    copied.method = request.method;
    copied.url = request.url;
    copied.timeout_ms = static_cast<int>(request.timeout_ms);
    copied.max_response_bytes = request.max_response_bytes;
    if (request.body.data == nullptr && request.body.size != 0) {
        throw Error(ErrorCode::PluginError,
                    "plugin replacement request body is invalid");
    }
    if (request.body.data != nullptr && request.body.size != 0) {
        copied.body.assign(request.body.data, request.body.size);
    }
    if (request.headers == nullptr && request.header_count != 0) {
        throw Error(ErrorCode::PluginError,
                    "plugin replacement request headers are invalid");
    }
    for (std::size_t i = 0; i < request.header_count; ++i) {
        const ProwseTkHeader& header = request.headers[i];
        if (header.name != nullptr && header.value != nullptr) {
            copied.headers.emplace_back(header.name, header.value);
        }
    }
    return copied;
}

}  // namespace

struct PluginRegistry::Impl {
    std::mutex mutex;
    std::vector<LoadedPlugin> plugins;
    ProwseTkHostApi host_api{};
    ProwseTkHost host{};
    void (*log_sink)(void*, int, const char*) = nullptr;
    void* log_user_data = nullptr;
    EventDispatcher* event_dispatcher = nullptr;
    Redactor redactor;

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

    static void emit_event_bridge(void* user_data, const char* type,
                                  const char* name, const char* message) {
        auto* impl = static_cast<Impl*>(user_data);
        if (impl == nullptr || impl->event_dispatcher == nullptr) {
            return;
        }
        Event event;
        const auto parsed = parse_event_type(string_or_empty(type));
        event.type = parsed.value_or(EventType::Console);
        event.name = string_or_empty(name);
        event.message = string_or_empty(message);
        if (!parsed.has_value() && type != nullptr) {
            event.attributes["plugin-event-type"] = type;
        }
        impl->event_dispatcher->emit(event);
    }

    static int redact_bridge(void* user_data, const char* value, char* output,
                             std::size_t output_size) {
        auto* impl = static_cast<Impl*>(user_data);
        if (impl == nullptr || value == nullptr || output == nullptr ||
            output_size == 0) {
            return PROWSETK_STATUS_INVALID_ARGUMENT;
        }
        const std::string redacted = impl->redactor.redact_url(value);
        if (redacted.size() + 1 > output_size) {
            return PROWSETK_STATUS_INVALID_ARGUMENT;
        }
        std::memcpy(output, redacted.c_str(), redacted.size() + 1);
        return PROWSETK_STATUS_OK;
    }

    ProwseTkHost* host_handle() {
        host_api.abi_version = PROWSETK_PLUGIN_ABI_VERSION;
        host_api.user_data = this;
        host_api.log = &Impl::log_bridge;
        host_api.emit_event = &Impl::emit_event_bridge;
        host_api.redact = &Impl::redact_bridge;
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
    if (std::string_view(info->name).empty()) {
        ::dlclose(handle);
        throw Error(ErrorCode::PluginError, "plugin name is empty");
    }
    if (!valid_plugin_type(info->type)) {
        ::dlclose(handle);
        throw Error(ErrorCode::PluginError, "plugin type is invalid");
    }
    if (info->abi_version == nullptr || abi_string() != info->abi_version) {
        const std::string reported_abi =
            info->abi_version != nullptr ? info->abi_version : "<null>";
        ::dlclose(handle);
        throw Error(ErrorCode::PluginError,
                    "plugin ABI mismatch: expected " + abi_string() + ", got " +
                        reported_abi);
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
    loaded.descriptor.lua_module = string_or_empty(info->lua_module);
    loaded.descriptor.wasm_world = string_or_empty(info->wasm_world);
    loaded.descriptor.type = info->type;
    for (std::size_t i = 0; i < info->capability_count; ++i) {
        if (info->capabilities != nullptr && info->capabilities[i] != nullptr) {
            const std::string capability = info->capabilities[i];
            if (!capability.empty()) {
                loaded.descriptor.capabilities.emplace_back(capability);
            }
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

std::size_t PluginRegistry::configure_all(
    const std::vector<std::pair<std::string, std::string>>& entries) {
    std::vector<ProwseTkConfigEntry> c_entries;
    c_entries.reserve(entries.size());
    for (const auto& [key, value] : entries) {
        c_entries.push_back(ProwseTkConfigEntry{key.c_str(), value.c_str()});
    }

    std::lock_guard<std::mutex> lock(impl_->mutex);
    std::size_t count = 0;
    for (auto& loaded : impl_->plugins) {
        if (!loaded.descriptor.initialized || loaded.plugin == nullptr ||
            loaded.plugin->configure == nullptr) {
            continue;
        }
        int rc = PROWSETK_STATUS_OK;
        try {
            rc = loaded.plugin->configure(impl_->host_handle(), c_entries.data(),
                                          c_entries.size());
        } catch (...) {
            rc = PROWSETK_STATUS_ERROR;
        }
        if (rc != PROWSETK_STATUS_OK) {
            throw Error(ErrorCode::PluginError,
                        "plugin failed to configure: " +
                            loaded.descriptor.name);
        }
        ++count;
    }
    return count;
}

void PluginRegistry::dispatch_before_request(HttpRequest& request) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (auto& loaded : impl_->plugins) {
        if (!loaded.descriptor.initialized || loaded.plugin == nullptr ||
            loaded.plugin->before_request == nullptr) {
            continue;
        }
        const auto headers = make_headers(request.headers);
        const ProwseTkHttpRequest c_request = make_c_request(request, headers);
        ProwseTkHookResult result{PROWSETK_HOOK_CONTINUE, nullptr, nullptr};
        int rc = PROWSETK_STATUS_OK;
        try {
            rc = loaded.plugin->before_request(impl_->host_handle(), &c_request,
                                               &result);
        } catch (...) {
            rc = PROWSETK_STATUS_ERROR;
        }
        if (rc != PROWSETK_STATUS_OK) {
            throw Error(ErrorCode::PluginError,
                        "plugin before_request failed: " +
                            loaded.descriptor.name);
        }
        if (result.action == PROWSETK_HOOK_CONTINUE) {
            continue;
        }
        if (result.action == PROWSETK_HOOK_REJECT) {
            throw Error(ErrorCode::SecurityViolation,
                        "request rejected by plugin " +
                            loaded.descriptor.name + ": " +
                            string_or_empty(result.message));
        }
        if (result.action == PROWSETK_HOOK_REPLACE_REQUEST) {
            if (result.replacement_request == nullptr) {
                throw Error(ErrorCode::PluginError,
                            "plugin replacement request is null: " +
                                loaded.descriptor.name);
            }
            request = copy_request(*result.replacement_request);
            continue;
        }
        throw Error(ErrorCode::PluginError,
                    "plugin returned an unknown hook action: " +
                        loaded.descriptor.name);
    }
}

void PluginRegistry::dispatch_after_response(const HttpRequest& request,
                                             const HttpResponse& response) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (auto& loaded : impl_->plugins) {
        if (!loaded.descriptor.initialized || loaded.plugin == nullptr ||
            loaded.plugin->after_response == nullptr) {
            continue;
        }
        const auto request_headers = make_headers(request.headers);
        const auto response_headers = make_headers(response.headers);
        const ProwseTkHttpRequest c_request =
            make_c_request(request, request_headers);
        const ProwseTkHttpResponse c_response =
            make_c_response(response, response_headers);
        ProwseTkHookResult result{PROWSETK_HOOK_CONTINUE, nullptr, nullptr};
        int rc = PROWSETK_STATUS_OK;
        try {
            rc = loaded.plugin->after_response(impl_->host_handle(), &c_request,
                                               &c_response, &result);
        } catch (...) {
            rc = PROWSETK_STATUS_ERROR;
        }
        if (rc != PROWSETK_STATUS_OK) {
            throw Error(ErrorCode::PluginError,
                        "plugin after_response failed: " +
                            loaded.descriptor.name);
        }
        if (result.action == PROWSETK_HOOK_REJECT) {
            throw Error(ErrorCode::SecurityViolation,
                        "response rejected by plugin " +
                            loaded.descriptor.name + ": " +
                            string_or_empty(result.message));
        }
        if (result.action != PROWSETK_HOOK_CONTINUE) {
            throw Error(ErrorCode::PluginError,
                        "plugin after_response returned an invalid action: " +
                            loaded.descriptor.name);
        }
    }
}

void PluginRegistry::dispatch_document(const Document& document) {
    const std::string url = document.url();
    const std::string title = document.title();
    const std::string html = document.html();
    const std::string text = document.text();
    const ProwseTkDocumentSnapshot snapshot{url.c_str(), title.c_str(),
                                            html.c_str(), text.c_str()};

    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (auto& loaded : impl_->plugins) {
        if (!loaded.descriptor.initialized || loaded.plugin == nullptr ||
            loaded.plugin->on_document == nullptr) {
            continue;
        }
        int rc = PROWSETK_STATUS_OK;
        try {
            rc = loaded.plugin->on_document(impl_->host_handle(), &snapshot);
        } catch (...) {
            rc = PROWSETK_STATUS_ERROR;
        }
        if (rc != PROWSETK_STATUS_OK) {
            throw Error(ErrorCode::PluginError,
                        "plugin on_document failed: " +
                            loaded.descriptor.name);
        }
    }
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
