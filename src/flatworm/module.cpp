#include "prowsetk/flatworm_module.hpp"

#include <algorithm>
#include <set>
#include <utility>

#if defined(__unix__) || defined(__APPLE__)
#include <dlfcn.h>
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include "flatworm/module_internal.hpp"
#include "prowsetk/error.hpp"

namespace prowsetk {
namespace {

std::string metadata(const char* text, std::size_t limit, bool required) {
    if (text == nullptr) {
        if (required) throw Error(ErrorCode::InvalidArgument, "module metadata is missing");
        return {};
    }
    std::size_t size = 0;
    while (size <= limit && text[size] != '\0') ++size;
    if (size > limit || (required && size == 0)) {
        throw Error(ErrorCode::InvalidArgument, "module metadata has invalid length");
    }
    return std::string(text, size);
}

bool identifier_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$';
}

std::string identifier(const char* text, bool module_name) {
    auto name = metadata(text, 64, true);
    if (!identifier_start(name.front()) ||
        !std::all_of(name.begin() + 1, name.end(), [module_name](char c) {
            return identifier_start(c) || (c >= '0' && c <= '9') ||
                   (module_name && c == '-');
        })) {
        throw Error(ErrorCode::InvalidArgument, "module or export name is invalid");
    }
    return name;
}

void check_value(const FlatwormValue& value) {
    if (value.type > FLATWORM_VALUE_JSON) {
        throw Error(ErrorCode::InvalidArgument, "module constant type is invalid");
    }
    if (value.type == FLATWORM_VALUE_STRING || value.type == FLATWORM_VALUE_JSON) {
        if (value.bytes.size > FLATWORM_MODULE_MAX_VALUE_BYTES) {
            throw Error(ErrorCode::ResourceLimit, "module constant byte limit exceeded");
        }
        if (value.bytes.data == nullptr && value.bytes.size != 0) {
            throw Error(ErrorCode::InvalidArgument, "module constant bytes are missing");
        }
    }
}

}  // namespace

struct FlatwormModule::Impl::Library {
#if defined(__unix__) || defined(__APPLE__)
    void* handle = nullptr;
    ~Library() { if (handle != nullptr) ::dlclose(handle); }
#elif defined(_WIN32)
    HMODULE handle = nullptr;
    ~Library() { if (handle != nullptr) ::FreeLibrary(handle); }
#endif
};

FlatwormModule::Impl::Impl(const FlatwormModuleDefinition& definition) {
    // Inspect the version/size prefix before reading any subsequent fields.
    if (definition.abi_version != FLATWORM_MODULE_ABI_VERSION ||
        definition.struct_size < sizeof(FlatwormModuleDefinition)) {
        throw Error(ErrorCode::InvalidArgument, "Flatworm module ABI mismatch");
    }
    info.name = identifier(definition.name, true);
    info.version = metadata(definition.version, 128, true);
    info.description = metadata(definition.description, 1024, false);
    if ((definition.initialize == nullptr) != (definition.shutdown == nullptr)) {
        throw Error(ErrorCode::InvalidArgument, "module lifecycle callbacks must be paired");
    }
    if (definition.function_count > FLATWORM_MODULE_MAX_EXPORTS ||
        definition.constant_count > FLATWORM_MODULE_MAX_EXPORTS - definition.function_count) {
        throw Error(ErrorCode::ResourceLimit, "module export limit exceeded");
    }
    if ((definition.function_count != 0 && definition.functions == nullptr) ||
        (definition.constant_count != 0 && definition.constants == nullptr)) {
        throw Error(ErrorCode::InvalidArgument, "module export table is missing");
    }
    initialize = definition.initialize;
    shutdown = definition.shutdown;
    std::set<std::string> names;
    const auto add_name = [&](const char* text) {
        auto name = identifier(text, false);
        if (!names.insert(name).second) {
            throw Error(ErrorCode::InvalidArgument, "duplicate module export");
        }
        info.exports.push_back(name);
        return name;
    };
    functions.reserve(definition.function_count);
    for (std::size_t i = 0; i < definition.function_count; ++i) {
        const auto& function = definition.functions[i];
        if (function.invoke == nullptr || function.arity > FLATWORM_MODULE_MAX_ARGUMENTS) {
            throw Error(ErrorCode::InvalidArgument, "module function is invalid");
        }
        functions.push_back({add_name(function.name), function.arity, function.invoke});
    }
    constants.reserve(definition.constant_count);
    std::size_t byte_count = 0;
    for (std::size_t i = 0; i < definition.constant_count; ++i) {
        const auto& constant = definition.constants[i];
        check_value(constant.value);
        Impl::Constant copied{add_name(constant.name), constant.value, {}};
        if (copied.value.type == FLATWORM_VALUE_STRING || copied.value.type == FLATWORM_VALUE_JSON) {
            if (copied.value.bytes.size > FLATWORM_MODULE_MAX_VALUE_BYTES - byte_count) {
                throw Error(ErrorCode::ResourceLimit, "module constant byte limit exceeded");
            }
            byte_count += copied.value.bytes.size;
            if (copied.value.bytes.size != 0) {
                copied.bytes.assign(copied.value.bytes.data, copied.value.bytes.size);
            }
            // Rebound to owned bytes when a runtime creates the constant.
            copied.value.bytes.data = nullptr;
        }
        constants.push_back(std::move(copied));
    }
}

FlatwormModule::FlatwormModule(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
FlatwormModule::~FlatwormModule() = default;
const FlatwormModuleInfo& FlatwormModule::info() const noexcept { return impl_->info; }

std::shared_ptr<const FlatwormModule> FlatwormModule::from_static(
    const FlatwormModuleDefinition& definition) {
    // make_shared's access checks require a public constructor on this enabler.
    struct Enabler : FlatwormModule {
        explicit Enabler(std::unique_ptr<Impl> impl) : FlatwormModule(std::move(impl)) {}
    };
    return std::make_shared<Enabler>(std::make_unique<Impl>(definition));
}

std::shared_ptr<const FlatwormModule> FlatwormModule::load_native(
    const std::filesystem::path& path) {
    auto library = std::make_shared<Impl::Library>();
    FlatwormModuleEntry entry = nullptr;
#if defined(__unix__) || defined(__APPLE__)
    library->handle = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (library->handle != nullptr) {
        entry = reinterpret_cast<FlatwormModuleEntry>(
            ::dlsym(library->handle, FLATWORM_MODULE_ENTRY_SYMBOL));
    }
#elif defined(_WIN32)
    library->handle = ::LoadLibraryW(path.c_str());
    if (library->handle != nullptr) {
        entry = reinterpret_cast<FlatwormModuleEntry>(
            ::GetProcAddress(library->handle, FLATWORM_MODULE_ENTRY_SYMBOL));
    }
#else
    (void)path;
    throw Error(ErrorCode::Unsupported, "native Flatworm modules are unavailable on this platform");
#endif
#if defined(__unix__) || defined(__APPLE__) || defined(_WIN32)
    if (library->handle == nullptr) {
        // Loader diagnostics can contain host paths/credentials; omit them.
        throw Error(ErrorCode::IoError, "failed to load native Flatworm module");
    }
    if (entry == nullptr) {
        throw Error(ErrorCode::InvalidArgument, "missing Flatworm module entry symbol");
    }
    const FlatwormModuleDefinition* definition = nullptr;
    try {
        definition = entry();
    } catch (...) {
        throw Error(ErrorCode::JavaScriptError, "Flatworm module entry failed");
    }
    if (definition == nullptr) {
        throw Error(ErrorCode::InvalidArgument, "Flatworm module entry returned no definition");
    }
    struct Enabler : FlatwormModule {
        explicit Enabler(std::unique_ptr<Impl> impl) : FlatwormModule(std::move(impl)) {}
    };
    auto impl = std::make_unique<Impl>(*definition);
    impl->library = std::move(library);
    return std::make_shared<Enabler>(std::move(impl));
#endif
}

void FlatwormModuleRegistry::add(std::shared_ptr<const FlatwormModule> module) {
    if (module == nullptr) throw Error(ErrorCode::InvalidArgument, "module is null");
    if (std::any_of(modules_.begin(), modules_.end(), [&](const auto& existing) {
            return existing->info().name == module->info().name;
        })) {
        throw Error(ErrorCode::InvalidArgument, "module is already registered");
    }
    if (modules_.size() == FLATWORM_MODULE_MAX_MODULES) {
        throw Error(ErrorCode::ResourceLimit, "module count limit exceeded");
    }
    modules_.push_back(std::move(module));
}

std::shared_ptr<const FlatwormModule> FlatwormModuleRegistry::load_native(
    const std::filesystem::path& path) {
    auto module = FlatwormModule::load_native(path);
    add(module);
    return module;
}

std::shared_ptr<const FlatwormModule> FlatwormModuleRegistry::register_static(
    const FlatwormModuleDefinition& definition) {
    auto module = FlatwormModule::from_static(definition);
    add(module);
    return module;
}

bool FlatwormModuleRegistry::remove(std::string_view name) {
    const auto found = std::find_if(modules_.begin(), modules_.end(), [&](const auto& module) {
        return module->info().name == name;
    });
    if (found == modules_.end()) return false;
    modules_.erase(found);
    return true;
}

std::vector<std::shared_ptr<const FlatwormModule>> FlatwormModuleRegistry::modules() const {
    return modules_;
}

}  // namespace prowsetk
