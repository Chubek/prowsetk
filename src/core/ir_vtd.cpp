// ir_vtd.cpp — encoding stratum (ProwseVTD binary token format).
//
// Sole owner of the VTD wire format. Consumes the canonical XAS event model
// produced by the semantics stratum; never walks the DOM and never assigns
// paths. Decoding is strict (bad magic, unknown token types, truncation, and
// trailing bytes all yield an empty vector) and allocation-bounded.

#include "ir_internal.hpp"

#include <algorithm>
#include <span>

namespace prowsetk {
namespace {

void write_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xffu));
    out.push_back(static_cast<std::uint8_t>((value >> 8u) & 0xffu));
    out.push_back(static_cast<std::uint8_t>((value >> 16u) & 0xffu));
    out.push_back(static_cast<std::uint8_t>((value >> 24u) & 0xffu));
}

bool read_u32(std::span<const std::uint8_t> bytes, std::size_t& pos,
              std::uint32_t& value) {
    if (bytes.size() - pos < 4) {
        return false;
    }
    value = static_cast<std::uint32_t>(bytes[pos]) |
            (static_cast<std::uint32_t>(bytes[pos + 1]) << 8u) |
            (static_cast<std::uint32_t>(bytes[pos + 2]) << 16u) |
            (static_cast<std::uint32_t>(bytes[pos + 3]) << 24u);
    pos += 4;
    return true;
}

bool read_vtd_string(std::span<const std::uint8_t> bytes, std::size_t& pos,
                     std::string& out) {
    std::uint32_t length = 0;
    if (!read_u32(bytes, pos, length) || bytes.size() - pos < length) {
        return false;
    }
    out.assign(reinterpret_cast<const char*>(bytes.data() + pos), length);
    pos += length;
    return true;
}

void write_bytes(std::vector<std::uint8_t>& out, std::string_view text) {
    write_u32(out, static_cast<std::uint32_t>(text.size()));
    out.insert(out.end(), text.begin(), text.end());
}

void write_token(std::vector<std::uint8_t>& out, std::uint8_t type,
                 std::uint32_t depth, std::string_view path,
                 std::string_view name, std::string_view value) {
    out.push_back(type);
    write_u32(out, depth);
    write_bytes(out, path);
    write_bytes(out, name);
    write_bytes(out, value);
}

}  // namespace

std::vector<std::uint8_t> emit_prowse_vtd(const Document& document) {
    const auto events = emit_prowse_xas(document);
    std::vector<std::uint8_t> binary;
    binary.insert(binary.end(), std::begin(kProwseVtdMagic),
                  std::end(kProwseVtdMagic));
    write_u32(binary, static_cast<std::uint32_t>(events.size()));

    for (const auto& event : events) {
        std::uint8_t type = 0;
        if (event.kind == "start") {
            type = kProwseVtdStart;
        } else if (event.kind == "attribute") {
            type = kProwseVtdAttribute;
        } else if (event.kind == "text") {
            type = kProwseVtdText;
        } else if (event.kind == "end") {
            type = kProwseVtdEnd;
        }
        write_token(binary, type, static_cast<std::uint32_t>(event.depth),
                    event.xpath, event.name.empty() ? event.tag : event.name,
                    event.value);
    }
    return binary;
}

std::vector<ProwseXasEvent> decode_prowse_vtd(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 9 || bytes[0] != 'P' || bytes[1] != 'V' ||
        bytes[2] != 'T' || bytes[3] != 'D' || bytes[4] != '1') {
        return {};
    }
    std::size_t pos = 5;
    std::uint32_t count = 0;
    if (!read_u32(bytes, pos, count)) {
        return {};
    }

    std::vector<ProwseXasEvent> events;
    // Bound the reservation by the input size: every token occupies at least
    // 13 bytes (type + depth + three empty strings), so a corrupt count can
    // never force a huge allocation from a small input.
    events.reserve((std::min<std::size_t>)(count, bytes.size() / 13 + 1));
    for (std::uint32_t i = 0; i < count; ++i) {
        if (pos >= bytes.size()) {
            return {};
        }
        const std::uint8_t type = bytes[pos++];
        std::uint32_t depth = 0;
        ProwseXasEvent event;
        if (!read_u32(bytes, pos, depth) ||
            !read_vtd_string(bytes, pos, event.xpath) ||
            !read_vtd_string(bytes, pos, event.name) ||
            !read_vtd_string(bytes, pos, event.value)) {
            return {};
        }
        event.depth = depth;
        switch (type) {
            case kProwseVtdStart:
                event.kind = "start";
                event.tag = event.name;
                event.name.clear();
                break;
            case kProwseVtdAttribute:
                // The wire format stores the attribute name in the name
                // field; the owning element tag is recovered from the layout
                // stratum path (an encoding concern, not a new layout).
                event.kind = "attribute";
                event.tag = ir::tag_from_path(event.xpath);
                break;
            case kProwseVtdText:
                event.kind = "text";
                event.tag = event.name;
                event.name.clear();
                break;
            case kProwseVtdEnd:
                event.kind = "end";
                event.tag = event.name;
                event.name.clear();
                break;
            default:
                return {};
        }
        events.push_back(std::move(event));
    }
    return pos == bytes.size() ? events : std::vector<ProwseXasEvent>{};
}

}  // namespace prowsetk
