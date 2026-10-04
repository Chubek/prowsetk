#include "module.hpp"

FLATWORM_MODULE_EXPORT const FlatwormModuleDefinition* flatworm_module_entry(void) {
    return &flatworm::rpc::module_definition();
}
