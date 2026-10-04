#include "Flatwork-Module.h"

#include <stdexcept>

// The header's declaration supplies C linkage even outside an extern-C block.
// Deliberately violate the callback exception rule to exercise containment.
FLATWORM_MODULE_EXPORT const FlatwormModuleDefinition* flatworm_module_entry(void) {
    throw std::runtime_error("private-entry-marker");
}
