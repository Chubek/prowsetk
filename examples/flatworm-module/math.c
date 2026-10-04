#include "Flatwork-Module.h"

static FlatwormStatus add(FlatwormCall* call) {
    FlatwormValue result = {0};
    if (call->argument_count != 2 ||
        call->arguments[0].type != FLATWORM_VALUE_NUMBER ||
        call->arguments[1].type != FLATWORM_VALUE_NUMBER) {
        return FLATWORM_STATUS_INVALID_ARGUMENT;
    }
    result.type = FLATWORM_VALUE_NUMBER;
    result.number = call->arguments[0].number + call->arguments[1].number;
    return call->api->set_result(call, &result);
}

static const FlatwormFunction functions[] = {{"add", 2, add}};
static const FlatwormConstant constants[] = {
    {"pi", {FLATWORM_VALUE_NUMBER, 0, 3.141592653589793, {NULL, 0}}}
};
static const FlatwormModuleDefinition module = {
    FLATWORM_MODULE_ABI_VERSION, sizeof(FlatwormModuleDefinition),
    "math", "1.0.0", "Example native JavaScript arithmetic",
    NULL, NULL, functions, 1, constants, 1
};

FLATWORM_MODULE_EXPORT const FlatwormModuleDefinition* flatworm_module_entry(void) {
    return &module;
}
