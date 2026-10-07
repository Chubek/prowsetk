#include "backend_internal.hpp"

namespace prowsetk::gfx {
namespace {
ProwseGfxStatus run(void* instance) noexcept {
    return instance ? PROWSETK_GFX_OK : PROWSETK_GFX_INVALID_ARGUMENT;
}
}
const ProwseGfxBackend& headless_backend() {
    static const ProwseGfxBackend definition{
        PROWSETK_GFX_ABI_VERSION, sizeof(ProwseGfxBackend), "headless",
        create, destroy, submit, run};
    return definition;
}
}
