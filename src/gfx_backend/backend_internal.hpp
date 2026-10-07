#ifndef PROWSETK_GFX_BACKEND_INTERNAL_HPP
#define PROWSETK_GFX_BACKEND_INTERNAL_HPP

#include "prowsetk/gfx_ir.hpp"
#include "GFX-Backend.h"
#include <memory>

namespace prowsetk::gfx {
struct State {
    GfxFrame frame;
};
inline ProwseGfxStatus create(const ProwseGfxSurface* surface, void** instance) noexcept {
    if (!instance) return PROWSETK_GFX_INVALID_ARGUMENT;
    *instance = nullptr;
    if (!surface) return PROWSETK_GFX_INVALID_ARGUMENT;
    try {
        auto state = std::make_unique<State>();
        state->frame.surface = {surface->width, surface->height};
        (void)encode_gfx_frame(state->frame);
        *instance = state.release();
        return PROWSETK_GFX_OK;
    } catch (...) { return PROWSETK_GFX_INVALID_ARGUMENT; }
}
inline void destroy(void* instance) noexcept {
    const std::unique_ptr<State> state(static_cast<State*>(instance));
}
inline ProwseGfxStatus submit(void* instance, const uint8_t* bytes, size_t size) noexcept {
    if (!instance || !bytes) return PROWSETK_GFX_INVALID_ARGUMENT;
    try {
        auto frame = decode_gfx_frame({bytes, size});
        auto& state = *static_cast<State*>(instance);
        if (frame.surface.width != state.frame.surface.width ||
            frame.surface.height != state.frame.surface.height) return PROWSETK_GFX_INVALID_ARGUMENT;
        state.frame = std::move(frame);
        return PROWSETK_GFX_OK;
    } catch (...) { return PROWSETK_GFX_INVALID_ARGUMENT; }
}
const ProwseGfxBackend& headless_backend();
}  // namespace prowsetk::gfx
#endif
