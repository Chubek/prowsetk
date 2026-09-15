#ifndef PROWSETK_PLUGIN_H
#define PROWSETK_PLUGIN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PROWSETK_PLUGIN_ABI_VERSION 1

/* Exported entry-point symbol. A shared object plugin defines:
 *
 *   PROWSETK_PLUGIN_EXPORT const ProwseTkPlugin* prowsetk_plugin_entry(void);
 */
#define PROWSETK_PLUGIN_ENTRY_SYMBOL "prowsetk_plugin_entry"

#if defined(_WIN32)
#define PROWSETK_PLUGIN_EXPORT __declspec(dllexport)
#else
#define PROWSETK_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

typedef struct ProwseTkHost ProwseTkHost;

typedef enum {
    PROWSETK_PLUGIN_NATIVE = 1,
    PROWSETK_PLUGIN_LUA = 2,
    PROWSETK_PLUGIN_WASM = 3
} ProwseTkPluginType;

typedef enum {
    PROWSETK_LOG_TRACE = 0,
    PROWSETK_LOG_DEBUG = 1,
    PROWSETK_LOG_INFO = 2,
    PROWSETK_LOG_WARN = 3,
    PROWSETK_LOG_ERROR = 4
} ProwseTkLogLevel;

/* Host services made available to plugins. The struct is owned by the host and
 * remains valid for the plugin's lifetime. */
typedef struct {
    uint32_t abi_version;
    void* user_data;
    void (*log)(void* user_data, int level, const char* message);
} ProwseTkHostApi;

struct ProwseTkHost {
    const ProwseTkHostApi* api;
};

typedef struct {
    const char* name;
    const char* version;
    const char* abi_version;
    const char* description;
    ProwseTkPluginType type;
    const char* const* capabilities;
    size_t capability_count;
} ProwseTkPluginInfo;

/* Return 0 on success, nonzero on failure. Implementations must not let C++
 * exceptions escape across this boundary. */
typedef struct {
    int (*initialize)(ProwseTkHost* host);
    void (*shutdown)(ProwseTkHost* host);
    const ProwseTkPluginInfo* (*info)(void);
} ProwseTkPlugin;

typedef const ProwseTkPlugin* (*ProwseTkPluginEntry)(void);

#ifdef __cplusplus
}
#endif

#endif /* PROWSETK_PLUGIN_H */
