#ifndef PROWSETK_FLATWORM_MODULE_HPP
#define PROWSETK_FLATWORM_MODULE_HPP

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "Flatworm-Module.hpp"

namespace prowsetk {
namespace flatworm { class ModuleBindings; }

struct FlatwormModuleInfo {
    std::string name;
    std::string version;
    std::string description;
    std::vector<std::string> exports;
};

// Immutable, validated native module definition and shared-library ownership.
// No page JavaScript executes and no module instance is created while loading.
class FlatwormModule {
public:
    static std::shared_ptr<const FlatwormModule> load_native(
        const std::filesystem::path& path);
    static std::shared_ptr<const FlatwormModule> from_static(
        const FlatwormModuleDefinition& definition);
    ~FlatwormModule();

    FlatwormModule(const FlatwormModule&) = delete;
    FlatwormModule& operator=(const FlatwormModule&) = delete;

    const FlatwormModuleInfo& info() const noexcept;

private:
    struct Impl;
    explicit FlatwormModule(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
    friend class flatworm::ModuleBindings;
};

// Host-side selection inherited by subsequently created runtimes/sessions.
// Serialize access just as for a Browser. Removing an entry does not invalidate
// installed bindings: each runtime retains its own shared library ownership.
class FlatwormModuleRegistry {
public:
    void add(std::shared_ptr<const FlatwormModule> module);
    std::shared_ptr<const FlatwormModule> load_native(
        const std::filesystem::path& path);
    std::shared_ptr<const FlatwormModule> register_static(
        const FlatwormModuleDefinition& definition);
    bool remove(std::string_view name);
    std::vector<std::shared_ptr<const FlatwormModule>> modules() const;

private:
    std::vector<std::shared_ptr<const FlatwormModule>> modules_;
};

}  // namespace prowsetk

#endif  // PROWSETK_FLATWORM_MODULE_HPP
