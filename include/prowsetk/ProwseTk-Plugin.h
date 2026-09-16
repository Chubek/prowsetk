#ifndef PROWSETK_PLUGIN_H
#define PROWSETK_PLUGIN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PROWSETK_PLUGIN_ABI_VERSION 2

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
    PROWSETK_STATUS_OK = 0,
    PROWSETK_STATUS_ERROR = 1,
    PROWSETK_STATUS_INVALID_ARGUMENT = 2,
    PROWSETK_STATUS_SECURITY_VIOLATION = 3
} ProwseTkStatus;

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

typedef enum {
    PROWSETK_HOOK_CONTINUE = 0,
    PROWSETK_HOOK_REJECT = 1,
    PROWSETK_HOOK_REPLACE_REQUEST = 2
} ProwseTkHookAction;

typedef struct {
    const char* data;
    size_t size;
} ProwseTkBytes;

typedef struct {
    const char* name;
    const char* value;
} ProwseTkHeader;

typedef struct {
    const char* method;
    const char* url;
    const ProwseTkHeader* headers;
    size_t header_count;
    ProwseTkBytes body;
    uint32_t timeout_ms;
    size_t max_response_bytes;
} ProwseTkHttpRequest;

typedef struct {
    uint16_t status;
    const ProwseTkHeader* headers;
    size_t header_count;
    ProwseTkBytes body;
    const char* final_url;
} ProwseTkHttpResponse;

typedef struct {
    ProwseTkHookAction action;
    const char* message;
    const ProwseTkHttpRequest* replacement_request;
} ProwseTkHookResult;

typedef struct {
    const char* key;
    const char* value;
} ProwseTkConfigEntry;

typedef struct {
    const char* url;
    const char* title;
    const char* html;
    const char* text;
} ProwseTkDocumentSnapshot;

/* Host services made available to plugins. The struct is owned by the host and
 * remains valid for the plugin's lifetime. */
typedef struct {
    uint32_t abi_version;
    void* user_data;
    void (*log)(void* user_data, int level, const char* message);
    void (*emit_event)(void* user_data, const char* type, const char* name,
                       const char* message);
    int (*redact)(void* user_data, const char* value, char* output,
                  size_t output_size);
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
    const char* lua_module;
    const char* wasm_world;
    const char* const* capabilities;
    size_t capability_count;
} ProwseTkPluginInfo;

/* Return 0 on success, nonzero on failure. Implementations must not let C++
 * exceptions escape across this boundary. */
typedef struct {
    int (*initialize)(ProwseTkHost* host);
    void (*shutdown)(ProwseTkHost* host);
    const ProwseTkPluginInfo* (*info)(void);
    int (*configure)(ProwseTkHost* host, const ProwseTkConfigEntry* entries,
                     size_t entry_count);
    int (*before_request)(ProwseTkHost* host, const ProwseTkHttpRequest* request,
                          ProwseTkHookResult* result);
    int (*after_response)(ProwseTkHost* host,
                          const ProwseTkHttpRequest* request,
                          const ProwseTkHttpResponse* response,
                          ProwseTkHookResult* result);
    int (*on_document)(ProwseTkHost* host,
                       const ProwseTkDocumentSnapshot* document);
} ProwseTkPlugin;

typedef const ProwseTkPlugin* (*ProwseTkPluginEntry)(void);

#ifdef __cplusplus
}
#endif

#endif /* PROWSETK_PLUGIN_H */
