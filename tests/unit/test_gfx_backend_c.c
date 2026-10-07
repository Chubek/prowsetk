#include "GFX-Backend.h"

static int token;
static int destroyed;
static int submitted;
static ProwseGfxStatus create(const ProwseGfxSurface* surface, void** instance) {
    if (!surface || !instance) return PROWSETK_GFX_INVALID_ARGUMENT;
    *instance = &token;
    return PROWSETK_GFX_OK;
}
static void destroy(void* instance) { if (instance == &token) ++destroyed; }
static ProwseGfxStatus submit(void* instance, const uint8_t* bytes, size_t size) {
    if (instance != &token || !bytes || size < 17) return PROWSETK_GFX_ERROR;
    ++submitted;
    return PROWSETK_GFX_OK;
}
static ProwseGfxStatus run(void* instance) {
    return instance == &token ? PROWSETK_GFX_OK : PROWSETK_GFX_ERROR;
}
const ProwseGfxBackend* test_gfx_backend(void) {
    static const ProwseGfxBackend backend = {
        PROWSETK_GFX_ABI_VERSION, sizeof(ProwseGfxBackend), "c-fixture", create, destroy, submit, run
    };
    destroyed = 0; submitted = 0;
    return &backend;
}
int test_gfx_destroyed(void) { return destroyed; }
int test_gfx_submitted(void) { return submitted; }
