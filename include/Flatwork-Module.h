#ifndef FLATWORK_MODULE_H
#define FLATWORK_MODULE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Native extensions of Flatworm's page JavaScript runtime. This ABI is
 * independent of ProwseTk-Plugin.h and does not expose QuickJS or DOM pointers.
 * A shared library exports:
 *   const FlatwormModuleDefinition* flatworm_module_entry(void);
 * All entry points and callbacks must contain their own C++ exceptions.
 */
#define FLATWORM_MODULE_ABI_VERSION 1u
#define FLATWORM_MODULE_ENTRY_SYMBOL "flatworm_module_entry"
#if defined(_WIN32)
#define FLATWORM_MODULE_EXPORT __declspec(dllexport)
#else
#define FLATWORM_MODULE_EXPORT __attribute__((visibility("default")))
#endif

#define FLATWORM_MODULE_MAX_MODULES 64u
#define FLATWORM_MODULE_MAX_EXPORTS 256u
#define FLATWORM_MODULE_MAX_ARGUMENTS 64u
/* Aggregate string/JSON argument bytes, and separately result bytes. */
#define FLATWORM_MODULE_MAX_VALUE_BYTES (1024u * 1024u)
#define FLATWORM_MODULE_MAX_CALL_DEPTH 32u

typedef enum {
    FLATWORM_STATUS_OK = 0,
    FLATWORM_STATUS_ERROR = 1,
    FLATWORM_STATUS_INVALID_ARGUMENT = 2,
    FLATWORM_STATUS_RESOURCE_LIMIT = 3
} FlatwormStatus;

typedef enum {
    FLATWORM_VALUE_UNDEFINED = 0,
    FLATWORM_VALUE_NULL = 1,
    FLATWORM_VALUE_BOOLEAN = 2,
    FLATWORM_VALUE_NUMBER = 3,
    FLATWORM_VALUE_STRING = 4,
    FLATWORM_VALUE_JSON = 5
} FlatwormValueType;

typedef struct {
    const char* data;
    size_t size;
} FlatwormBytes;

/* Only the field selected by type is used. Strings are length-delimited UTF-8
 * (embedded NULs are allowed). JSON uses standard JSON serialization, including
 * toJSON/getters on JavaScript arguments; functions, symbols and BigInts are
 * rejected as top-level arguments. No engine value or pointer crosses the ABI.
 */
typedef struct {
    uint32_t type; /* FlatwormValueType code; unknown codes are rejected */
    int boolean;
    double number;
    FlatwormBytes bytes;
} FlatwormValue;

typedef struct FlatwormCall FlatwormCall;

typedef struct {
    uint32_t abi_version;
    size_t struct_size;
    /* Copies the value before returning. The default result is undefined.
     * Multiple successful calls replace the result. An invalid result poisons
     * the call even if the native function ignores the returned error status.
     * Neither call nor its host_data may be retained after the callback.
     */
    FlatwormStatus (*set_result)(FlatwormCall* call, const FlatwormValue* value);
} FlatwormModuleHostApi;

struct FlatwormCall {
    const FlatwormModuleHostApi* api;
    void* instance;
    const FlatwormValue* arguments;
    size_t argument_count;
    void* host_data; /* opaque; only pass it back through the host API */
};

typedef FlatwormStatus (*FlatwormNativeFunction)(FlatwormCall* call);

typedef struct {
    const char* name;
    uint32_t arity; /* JavaScript Function.length; validate actual arguments */
    FlatwormNativeFunction invoke;
} FlatwormFunction;

typedef struct {
    const char* name;
    FlatwormValue value;
} FlatwormConstant;

typedef struct {
    uint32_t abi_version;
    size_t struct_size; /* set to sizeof(FlatwormModuleDefinition) */
    /* ASCII [A-Za-z_$][A-Za-z0-9_$-]*, at most 64 bytes. Export names use the
     * same rule without '-'. All metadata and export names are NUL-terminated.
     */
    const char* name;
    const char* version;     /* at most 128 bytes */
    const char* description; /* at most 1024 bytes; may be NULL */
    /* Called once per installed runtime, on its owning thread. Set *instance
     * to module-owned state. shutdown is called exactly once after an attempted
     * initialize, including failure, and must accept NULL/partial state. Both
     * callbacks may be NULL only for stateless modules. The host API remains
     * valid until shutdown. Callbacks are synchronous; never reenter the same
     * runtime or open sockets on behalf of a page. Native code is trusted and
     * is not interrupted or memory-sandboxed by JavaScript resource limits.
     */
    FlatwormStatus (*initialize)(const FlatwormModuleHostApi* api,
                                 void** instance);
    void (*shutdown)(void* instance);
    const FlatwormFunction* functions;
    size_t function_count;
    const FlatwormConstant* constants;
    size_t constant_count;
} FlatwormModuleDefinition;

/* Return immutable metadata; loading alone never calls initialize. Metadata
 * and constant bytes are copied by the host. Static callback code must remain
 * valid until the last runtime using it is destroyed. Shared-library code is
 * retained automatically until every installed runtime has shut down.
 * Callback failures become generic, argument-free JavaScript errors.
 */
typedef const FlatwormModuleDefinition* (*FlatwormModuleEntry)(void);

/* Implemented by the module library, not by the engine. This declaration
 * gives C++ definitions C linkage; add FLATWORM_MODULE_EXPORT to the definition.
 */
const FlatwormModuleDefinition* flatworm_module_entry(void);

#ifdef __cplusplus
}
#endif

#endif /* FLATWORK_MODULE_H */
