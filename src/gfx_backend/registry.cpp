#include "prowsetk/gfx_backend.hpp"
#include "prowsetk/error.hpp"
#include "backend_internal.hpp"
#include "X11-GUI.hpp"
#include "FLTK-GUI.hpp"

namespace prowsetk {
namespace {
const ProwseGfxBackend& lookup(std::string_view name) {
    if (name == "headless") return gfx::headless_backend();
#ifdef PROWSETK_GFX_HAVE_X11
    if (name == "x11") return gfx::x11_backend();
#endif
#ifdef PROWSETK_GFX_HAVE_FLTK
    if (name == "fltk") return gfx::fltk_backend();
#endif
    for (const auto& backend : gfx_backends())
        if (backend.name == name)
            throw Error(ErrorCode::Unsupported, "GFX backend is unavailable in this build");
    throw Error(ErrorCode::InvalidArgument, "unknown GFX backend");
}
void status(ProwseGfxStatus value) {
    if (value != PROWSETK_GFX_OK)
        throw Error(value == PROWSETK_GFX_UNAVAILABLE ? ErrorCode::Unsupported : ErrorCode::PluginError,
                    "GFX backend operation failed");
}
}

std::vector<GfxBackendInfo> gfx_backends() {
    return {{"headless", true}, {"x11",
#ifdef PROWSETK_GFX_HAVE_X11
        true
#else
        false
#endif
    }, {"fltk",
#ifdef PROWSETK_GFX_HAVE_FLTK
        true
#else
        false
#endif
    }, {"bgfx", false}, {"imgui", false}, {"wayland", false}};
}

std::string_view default_gfx_backend() noexcept {
#ifdef PROWSETK_GFX_HAVE_FLTK
    return "fltk";
#elif defined(PROWSETK_GFX_HAVE_X11)
    return "x11";
#else
    return "headless";
#endif
}

struct GfxBackend::Impl {
    const ProwseGfxBackend* definition;
    void* instance = nullptr;
    GfxOptions surface;
    ~Impl() {
        if (instance) {
            try { definition->destroy(instance); } catch (...) { /* C ABI containment. */ }
        }
    }
};

GfxBackend::GfxBackend(std::string_view name, GfxOptions options)
    : GfxBackend(lookup(name), options) {}

GfxBackend::GfxBackend(const ProwseGfxBackend& definition, GfxOptions options) {
    if (definition.abi_version != PROWSETK_GFX_ABI_VERSION ||
        definition.struct_size < sizeof(ProwseGfxBackend) || !definition.name || !definition.name[0] ||
        !definition.create || !definition.destroy || !definition.submit || !definition.run)
        throw Error(ErrorCode::InvalidArgument, "invalid GFX backend definition");
    (void)encode_gfx_frame({options, {}});
    auto impl = std::make_unique<Impl>();
    impl->definition = &definition; impl->surface = options;
    const ProwseGfxSurface surface{options.width, options.height};
    ProwseGfxStatus result;
    try { result = definition.create(&surface, &impl->instance); }
    catch (...) { throw Error(ErrorCode::PluginError, "GFX backend creation failed"); }
    status(result);
    if (!impl->instance) throw Error(ErrorCode::PluginError, "GFX backend creation failed");
    impl_ = std::move(impl);
}
GfxBackend::~GfxBackend() = default;

void GfxBackend::submit(std::span<const std::uint8_t> bytes) {
    const auto frame = decode_gfx_frame(bytes);
    if (frame.surface.width != impl_->surface.width || frame.surface.height != impl_->surface.height)
        throw Error(ErrorCode::InvalidArgument, "GFX frame surface mismatch");
    ProwseGfxStatus result;
    try { result = impl_->definition->submit(impl_->instance, bytes.data(), bytes.size()); }
    catch (...) { throw Error(ErrorCode::PluginError, "GFX backend submission failed"); }
    status(result);
}
void GfxBackend::run() {
    ProwseGfxStatus result;
    try { result = impl_->definition->run(impl_->instance); }
    catch (...) { throw Error(ErrorCode::PluginError, "GFX backend execution failed"); }
    status(result);
}
}  // namespace prowsetk
