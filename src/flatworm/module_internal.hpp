#ifndef PROWSETK_FLATWORM_MODULE_INTERNAL_HPP
#define PROWSETK_FLATWORM_MODULE_INTERNAL_HPP

#include "prowsetk/flatworm_module.hpp"

namespace prowsetk {

struct FlatwormModule::Impl {
    struct Library;
    struct Function {
        std::string name;
        uint32_t arity;
        FlatwormNativeFunction invoke;
    };
    struct Constant {
        std::string name;
        FlatwormValue value;
        std::string bytes;
    };

    explicit Impl(const FlatwormModuleDefinition& definition);

    std::shared_ptr<Library> library;
    FlatwormModuleInfo info;
    decltype(FlatwormModuleDefinition::initialize) initialize = nullptr;
    decltype(FlatwormModuleDefinition::shutdown) shutdown = nullptr;
    std::vector<Function> functions;
    std::vector<Constant> constants;
};

}  // namespace prowsetk

#endif  // PROWSETK_FLATWORM_MODULE_INTERNAL_HPP
