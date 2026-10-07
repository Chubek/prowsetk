#ifndef PROWSETK_GFX_BACKEND_H
#define PROWSETK_GFX_BACKEND_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PROWSETK_GFX_ABI_VERSION 1u
#define PROWSETK_GFX_ENTRY_SYMBOL "prowsetk_gfx_backend_entry"

typedef enum ProwseGfxStatus {
    PROWSETK_GFX_OK = 0,
    PROWSETK_GFX_ERROR = 1,
    PROWSETK_GFX_INVALID_ARGUMENT = 2,
    PROWSETK_GFX_UNAVAILABLE = 3
} ProwseGfxStatus;

typedef struct ProwseGfxSurface {
    uint32_t width;
    uint32_t height;
} ProwseGfxSurface;

/* Versioned, independent graphics ABI. No DOM, C++ or toolkit handles cross it.
 * All calls are synchronous on the creating thread and must not throw.
 * create transfers an opaque instance to the caller on success; destroy releases
 * it exactly once. On failure create must set *instance to NULL and clean up.
 * submit borrows PGFX1 bytes only for the duration of the call. It must validate
 * the complete frame and replace its previous frame atomically on success.
 * run opens/pumps the chosen surface until closed (headless returns immediately).
 * The definition and its shared library must outlive all instances.
 * Network activity and application/session control belong to the host tool.
 */
typedef struct ProwseGfxBackend {
    uint32_t abi_version;
    size_t struct_size;
    const char* name;
    ProwseGfxStatus (*create)(const ProwseGfxSurface* surface, void** instance);
    void (*destroy)(void* instance);
    ProwseGfxStatus (*submit)(void* instance, const uint8_t* bytes, size_t size);
    ProwseGfxStatus (*run)(void* instance);
} ProwseGfxBackend;

typedef const ProwseGfxBackend* (*ProwseGfxBackendEntry)(void);

#ifdef __cplusplus
}
#endif
#endif
