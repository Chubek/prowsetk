#include "prowsetk/gfx_ir.hpp"
#include "prowsetk/error.hpp"

#include <algorithm>
#include <string_view>

namespace prowsetk {
namespace {
[[noreturn]] void invalid() {
    throw Error(ErrorCode::ParseError, "invalid GFX bytecode");
}
void dimensions(GfxOptions options) {
    if (!options.width || !options.height || options.width > max_gfx_dimension ||
        options.height > max_gfx_dimension)
        throw Error(ErrorCode::InvalidArgument, "invalid GFX surface dimensions");
}
bool valid_text(std::string_view text) {
    for (std::size_t i = 0; i < text.size();) {
        const auto c = static_cast<unsigned char>(text[i++]);
        if (c < 32 || c == 127) return false;
        if (c < 128) continue;
        const unsigned n = c >= 0xc2 && c <= 0xdf ? 1u :
            c >= 0xe0 && c <= 0xef ? 2u : c >= 0xf0 && c <= 0xf4 ? 3u : 0u;
        if (!n || text.size() - i < n) return false;
        const auto first = static_cast<unsigned char>(text[i]);
        if ((c == 0xe0 && first < 0xa0) || (c == 0xed && first >= 0xa0) ||
            (c == 0xf0 && first < 0x90) || (c == 0xf4 && first >= 0x90)) return false;
        for (unsigned j = 0; j < n; ++j)
            if ((static_cast<unsigned char>(text[i++]) & 0xc0u) != 0x80u) return false;
    }
    return true;
}
void check(const GfxText& command, GfxOptions surface) {
    if (command.x >= surface.width || command.y > max_gfx_content_height ||
        command.text.size() > max_gfx_text_bytes || !valid_text(command.text)) invalid();
}
void u32(GfxBytecode& out, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8)
        out.push_back(static_cast<std::uint8_t>(value >> shift));
}
}

GfxBytecode encode_gfx_frame(const GfxFrame& frame) {
    dimensions(frame.surface);
    if (frame.text.size() > max_gfx_commands)
        throw Error(ErrorCode::ResourceLimit, "GFX command limit exceeded");
    std::size_t size = 17;
    for (const auto& command : frame.text) {
        check(command, frame.surface);
        if (13 + command.text.size() > max_gfx_bytes - size)
            throw Error(ErrorCode::ResourceLimit, "GFX byte limit exceeded");
        size += 13 + command.text.size();
    }
    GfxBytecode bytes{'P', 'G', 'F', 'X', '1'};
    bytes.reserve(size);
    u32(bytes, frame.surface.width); u32(bytes, frame.surface.height);
    u32(bytes, static_cast<std::uint32_t>(frame.text.size()));
    for (const auto& command : frame.text) {
        bytes.push_back(1); u32(bytes, command.x); u32(bytes, command.y);
        u32(bytes, static_cast<std::uint32_t>(command.text.size()));
        bytes.insert(bytes.end(), command.text.begin(), command.text.end());
    }
    return bytes;
}

GfxFrame decode_gfx_frame(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 17 || bytes.size() > max_gfx_bytes ||
        !std::equal(bytes.begin(), bytes.begin() + 5, "PGFX1")) invalid();
    std::size_t cursor = 5;
    const auto read = [&]() {
        if (bytes.size() - cursor < 4) invalid();
        std::uint32_t value = 0;
        for (unsigned shift = 0; shift < 32; shift += 8)
            value |= static_cast<std::uint32_t>(bytes[cursor++]) << shift;
        return value;
    };
    GfxFrame frame;
    frame.surface.width = read(); frame.surface.height = read();
    try { dimensions(frame.surface); } catch (const Error&) { invalid(); }
    const auto count = read();
    if (count > max_gfx_commands || count > (bytes.size() - cursor) / 13) invalid();
    frame.text.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        if (cursor == bytes.size() || bytes[cursor++] != 1) invalid();
        GfxText command;
        command.x = read(); command.y = read();
        const auto length = read();
        if (length > max_gfx_text_bytes || length > bytes.size() - cursor) invalid();
        command.text.assign(reinterpret_cast<const char*>(bytes.data() + cursor), length);
        cursor += length;
        check(command, frame.surface);
        frame.text.push_back(std::move(command));
    }
    if (cursor != bytes.size()) invalid();
    return frame;
}
}  // namespace prowsetk
