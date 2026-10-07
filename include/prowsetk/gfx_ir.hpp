#ifndef PROWSETK_GFX_IR_HPP
#define PROWSETK_GFX_IR_HPP

#include <cstdint>
#include <span>
#include <string>
#include <vector>
#include "prowsetk/event.hpp"

namespace prowsetk {

inline constexpr std::size_t max_gfx_bytes = 4u * 1024u * 1024u;
inline constexpr std::size_t max_gfx_commands = 50000;
inline constexpr std::size_t max_gfx_text_bytes = 4096;
inline constexpr std::uint32_t max_gfx_dimension = 16384;
inline constexpr std::uint32_t max_gfx_content_height = 1000000;

struct GfxOptions {
    std::uint32_t width = 800;
    std::uint32_t height = 600;
};
struct GfxText {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::string text;
    bool operator==(const GfxText&) const = default;
};
struct GfxFrame {
    GfxOptions surface;
    std::vector<GfxText> text;
};
using GfxBytecode = std::vector<std::uint8_t>;

// PGFX1: five magic bytes, LE u32 width/height/command count, followed by
// opcode 1 (text), LE u32 x/y/UTF-8 byte length, and that many text bytes.
// Coordinates are document pixels, top-left origin, fixed 8px/18px preview
// cells. Backends clear white and draw black text; y may exceed surface height
// for scrolling. Unknown opcodes, invalid UTF-8, trailing bytes and bounds fail.
GfxBytecode encode_gfx_frame(const GfxFrame& frame);
GfxFrame decode_gfx_frame(std::span<const std::uint8_t> bytes);

// Lowers only canonical events, never DOM or HTML. A bounded text preview with
// semantic block breaks; script/style/head/template, hidden subtrees and private
// control contents/values are excluded. Other page text remains caller data.
// This is independent of render_document's CSS display-list snapshot API.
GfxBytecode emit_gfx_ir(std::span<const ProwseEvent> events,
                       GfxOptions options = {});

}  // namespace prowsetk
#endif
