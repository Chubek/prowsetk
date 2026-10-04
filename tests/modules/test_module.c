#include "Flatwork-Module.h"

#include <stdlib.h>

typedef struct { double counter; } TestInstance;

static FlatwormStatus initialize(const FlatwormModuleHostApi* api, void** instance) {
    if (api->abi_version != FLATWORM_MODULE_ABI_VERSION ||
        api->struct_size < sizeof(FlatwormModuleHostApi) || api->set_result == NULL) {
        return FLATWORM_STATUS_ERROR;
    }
    *instance = calloc(1, sizeof(TestInstance));
    return *instance != NULL ? FLATWORM_STATUS_OK : FLATWORM_STATUS_ERROR;
}

static void shutdown(void* instance) { free(instance); }

static FlatwormStatus echo(FlatwormCall* call) {
    if (call->argument_count != 1) return FLATWORM_STATUS_INVALID_ARGUMENT;
    return call->api->set_result(call, &call->arguments[0]);
}

static FlatwormStatus next(FlatwormCall* call) {
    TestInstance* instance = (TestInstance*)call->instance;
    FlatwormValue value = {0};
    if (call->argument_count != 0 || instance == NULL) return FLATWORM_STATUS_INVALID_ARGUMENT;
    value.type = FLATWORM_VALUE_NUMBER;
    value.number = ++instance->counter;
    return call->api->set_result(call, &value);
}

static const FlatwormFunction functions[] = {{"echo", 1, echo}, {"next", 0, next}};
static const FlatwormConstant constants[] = {
    {"answer", {FLATWORM_VALUE_NUMBER, 0, 42, {NULL, 0}}},
    {"ready", {FLATWORM_VALUE_BOOLEAN, 1, 0, {NULL, 0}}},
    {"label", {FLATWORM_VALUE_STRING, 0, 0, {"native", 6}}},
    {"schema", {FLATWORM_VALUE_JSON, 0, 0, {"{\"kind\":\"fixture\"}", 18}}}
};

static const FlatwormModuleDefinition definition = {
#ifdef TEST_MODULE_BAD_ABI
    FLATWORM_MODULE_ABI_VERSION + 1,
#else
    FLATWORM_MODULE_ABI_VERSION,
#endif
    sizeof(FlatwormModuleDefinition), "fixture", "1.0.0", "C ABI fixture",
    initialize, shutdown, functions, sizeof(functions) / sizeof(functions[0]),
    constants, sizeof(constants) / sizeof(constants[0])
};

FLATWORM_MODULE_EXPORT const FlatwormModuleDefinition* flatworm_module_entry(void) {
    return &definition;
}
