#ifndef PROWSETK_GFX_BACKEND_HPP
#define PROWSETK_GFX_BACKEND_HPP

#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include "GFX-Backend.h"
#include "prowsetk/gfx_ir.hpp"

namespace prowsetk {

struct GfxBackendInfo {
    std::string name;
    bool available = false;
};
std::vector<GfxBackendInfo> gfx_backends();
std::string_view default_gfx_backend() noexcept;

// RAII client of the graphics C ABI. Custom definitions are explicitly supplied
// by trusted hosts; their definition/library must remain alive. Built-in lookup
// never opens a display. submit validates bytecode before invoking the backend.
class GfxBackend {
public:
    explicit GfxBackend(std::string_view name, GfxOptions options = {});
    explicit GfxBackend(const ProwseGfxBackend& definition, GfxOptions options = {});
    ~GfxBackend();
    GfxBackend(const GfxBackend&) = delete;
    GfxBackend& operator=(const GfxBackend&) = delete;
    void submit(std::span<const std::uint8_t> bytes);
    void run();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace prowsetk
#endif
