// text_metrics.cpp — text stratum: measurement and line breaking.
//
// See text_metrics.hpp for the stratum contract. Two things live here and
// nothing else: how wide a run is, and where a run may break. Neither touches
// the DOM, the cascade, or geometry.

#include "core/render/text_metrics.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <prowsetk/error.hpp>

namespace prowsetk::detail {
namespace {

// Unicode ranges treated as wide (CJK, Hangul, Kana, fullwidth forms, and the
// emoji blocks that behave like them). A run in these advances about twice the
// font size. This is a width model, not a segmentation algorithm, and it is the
// same approximation real engines use when no shaping data is available.
bool is_wide_code_point(std::uint32_t cp) {
    static constexpr std::array<std::pair<std::uint32_t, std::uint32_t>, 26> kWide{{
        {0x1100, 0x115f},   {0x2e80, 0x303e},   {0x3041, 0x33ff},
        {0x3400, 0x4dbf},   {0x4e00, 0x9fff},   {0xa000, 0xa4cf},
        {0xa960, 0xa97f},   {0xac00, 0xd7a3},   {0xf900, 0xfaff},
        {0xfe10, 0xfe19},   {0xfe30, 0xfe6f},   {0xff00, 0xff60},
        {0xffe0, 0xffe6},   {0x1f004, 0x1f004}, {0x1f0cf, 0x1f0cf},
        {0x1f18e, 0x1f18e}, {0x1f191, 0x1f19a}, {0x1f200, 0x1f320},
        {0x1f330, 0x1f335}, {0x1f337, 0x1f37c}, {0x1f380, 0x1f393},
        {0x1f3a0, 0x1f3ca}, {0x1f3cf, 0x1f3d3}, {0x1f3e0, 0x1f3f0},
        {0x1f400, 0x1f43e}, {0x1f440, 0x1f440},
    }};
    for (const auto& [low, high] : kWide) {
        if (cp >= low && cp <= high) return true;
    }
    return false;
}

// Decodes one UTF-8 code point, advancing `i`. Invalid bytes are returned as
// U+FFFD and consume one byte, so a mis-encoded page cannot desynchronize the
// scanner or loop forever.
std::uint32_t next_code_point(std::string_view text, std::size_t& i) {
    const auto byte = static_cast<unsigned char>(text[i]);
    if (byte < 0x80) { ++i; return byte; }
    std::size_t extra = 0;
    std::uint32_t cp = 0;
    if ((byte & 0xe0u) == 0xc0u) { extra = 1; cp = byte & 0x1fu; }
    else if ((byte & 0xf0u) == 0xe0u) { extra = 2; cp = byte & 0x0fu; }
    else if ((byte & 0xf8u) == 0xf0u) { extra = 3; cp = byte & 0x07u; }
    else { ++i; return 0xfffd; }
    if (i + extra >= text.size()) { ++i; return 0xfffd; }
    for (std::size_t k = 1; k <= extra; ++k) {
        const auto next = static_cast<unsigned char>(text[i + k]);
        if ((next & 0xc0u) != 0x80u) { ++i; return 0xfffd; }
        cp = (cp << 6) | (next & 0x3fu);
    }
    // Reject overlong forms, surrogates and out-of-range values.
    static constexpr std::uint32_t kMinimum[4] = {0u, 0x80u, 0x800u, 0x10000u};
    const bool valid = cp >= kMinimum[extra] && cp <= 0x10ffffu &&
                       !(cp >= 0xd800u && cp <= 0xdfffu);
    i += extra + 1;
    return valid ? cp : 0xfffd;
}

// Advance of one code point, in em units of the font size.
double code_point_advance(std::uint32_t cp, bool monospace, bool bold) {
    if (cp == '\t') return 4.0;
    if (cp == ' ') return monospace ? 1.0 : 0.28;
    if (cp < 0x20) return 0.0;  // other C0 controls never contribute
    if (is_wide_code_point(cp)) return 1.0;
    if (monospace) return 1.0;
    // Narrow ASCII, weighted to approximate a proportional face closely enough
    // that line breaking lands in the right place for typical page text.
    if (cp >= 'a' && cp <= 'z') return 0.53;
    if (cp >= 'A' && cp <= 'Z') return 0.68;
    if (cp >= '0' && cp <= '9') return 0.56;
    switch (cp) {
        case 'i': case 'l': case 'j': case 't': case 'f': case 'I':
            return 0.30;
        case 'm': case 'w': case 'M': case 'W':
            return 0.85;
        case '.': case ',': case ':': case ';': case '\'': case '`':
            return 0.27;
        case 'r': case 's': case 'z': case '/':
            return 0.42;
        case ' ': return 0.28;
        default: break;
    }
    // Punctuation and other scripts: a middle value between narrow and wide.
    return bold ? 0.60 : 0.56;
}

std::string lowered(std::string_view text) {
    std::string out(text);
    for (auto& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

}  // namespace

bool is_monospace_family(std::string_view family) {
    static constexpr std::array<std::string_view, 9> kMonospace{
        "monospace", "courier", "courier new", "consolas", "menlo",
        "monaco", "dejavu sans mono", "liberation mono", "ui-monospace",
    };
    const auto key = lowered(family);
    return std::find(kMonospace.begin(), kMonospace.end(), key) != kMonospace.end();
}

FontMetrics ApproximateTextMeasurer::metrics(const FontSpec& font) const {
    FontMetrics result;
    const double size = (font.size > 0.0 && std::isfinite(font.size)) ? font.size : 16.0;
    const bool bold = font.weight >= 600;
    // Ascent/descent track a typical Latin face; `bold` adds a little so a
    // heading's line box is slightly taller, as in a real face.
    result.ascent = size * (bold ? 0.82 : 0.80);
    result.descent = size * (bold ? 0.24 : 0.22);
    result.line_gap = size * 0.06;
    result.line_height = result.ascent + result.descent + result.line_gap;
    const bool monospace = is_monospace_family(font.family);
    result.space_advance = size * code_point_advance(' ', monospace, bold);
    result.average_advance = size * code_point_advance('n', monospace, bold);
    return result;
}

double ApproximateTextMeasurer::measure(std::string_view text,
                                        const FontSpec& font) const {
    const double size = (font.size > 0.0 && std::isfinite(font.size)) ? font.size : 16.0;
    const bool monospace = is_monospace_family(font.family);
    const bool bold = font.weight >= 600;
    double em = 0.0;
    std::size_t i = 0;
    while (i < text.size()) {
        const auto cp = next_code_point(text, i);
        em += code_point_advance(cp, monospace, bold);
    }
    const double width = em * size;
    return (width > 0.0 && std::isfinite(width)) ? width : 0.0;
}

double resolve_line_height(double font_size, double line_height,
                           const FontMetrics& metrics) {
    if (!std::isfinite(font_size) || font_size <= 0.0) return 0.0;
    if (line_height > 0.0 && std::isfinite(line_height)) return line_height;
    if (metrics.empty()) return font_size * 1.2;
    return metrics.line_height;
}

std::vector<BreakOpportunity> find_breaks(std::string_view text,
                                          WhiteSpace white_space) {
    std::vector<BreakOpportunity> breaks;
    const bool preserves = white_space == WhiteSpace::Pre ||
                           white_space == WhiteSpace::PreWrap ||
                           white_space == WhiteSpace::PreLine;
    const bool collapses = white_space == WhiteSpace::Pre ||
                           white_space == WhiteSpace::NoWrap ||
                           white_space == WhiteSpace::PreWrap;
    if (white_space == WhiteSpace::NoWrap) return breaks;
    std::size_t i = 0;
    while (i < text.size()) {
        const auto start = i;
        const auto cp = next_code_point(text, i);
        if (cp == '\n' || cp == '\r') {
            // A hard break always exists. `\r\n` is one break.
            if (cp == '\r' && i < text.size() && text[i] == '\n') ++i;
            breaks.push_back({i, false});
            continue;
        }
        if (cp == ' ' || cp == '\t' || cp == '\f' || cp == 0x0b) {
            // The break sits after the whole run of collapsible whitespace, and
            // consuming it is what removes it from the start of the next line.
            while (i < text.size()) {
                std::size_t probe = i;
                const auto next = next_code_point(text, probe);
                if (next != ' ' && next != '\t' && next != '\f' && next != 0x0b) break;
                i = probe;
            }
            breaks.push_back({i, collapses});
            continue;
        }
        // A line may also break between two CJK ideographs, where there is no
        // whitespace to break at.
        (void)start;
        if (preserves || collapses) continue;
        if (is_wide_code_point(cp) && i < text.size()) {
            std::size_t probe = i;
            const auto next = next_code_point(text, probe);
            if (next != ' ' && next != '\t' && next != '\n' && next != '\r' &&
                is_wide_code_point(next)) {
                breaks.push_back({i, false});
            }
        }
    }
    return breaks;
}

}  // namespace prowsetk::detail

namespace prowsetk {

const TextMeasurer& default_text_measurer() {
    static const detail::ApproximateTextMeasurer measurer;
    return measurer;
}

std::vector<std::pair<std::size_t, std::size_t>> break_text(
    std::string_view text, const FontSpec& font, double max_width,
    WhiteSpace white_space, const TextMeasurer& measurer) {
    std::vector<std::pair<std::size_t, std::size_t>> lines;
    if (text.empty()) return lines;
    if (!(max_width > 0.0)) { lines.emplace_back(0, text.size()); return lines; }

    if (text.size() > 16u * 1024u * 1024u)
        throw Error(ErrorCode::ResourceLimit, "render text budget");
    const bool wrap = white_space != WhiteSpace::Pre && white_space != WhiteSpace::NoWrap;
    const bool hard = white_space == WhiteSpace::Pre || white_space == WhiteSpace::PreWrap || white_space == WhiteSpace::PreLine;
    std::size_t start = 0, i = 0, soft = 0;
    double used = 0;
    const auto emit = [&](std::size_t end) {
        if (lines.size() >= max_render_paint_items)
            throw Error(ErrorCode::ResourceLimit, "render line budget");
        lines.emplace_back(start, end);
    };
    while (i < text.size()) {
        if (hard && (text[i] == '\n' || text[i] == '\r')) {
            emit(i);
            const char c = text[i++];
            if (c == '\r' && i < text.size() && text[i] == '\n') ++i;
            start = i; soft = 0; used = 0; continue;
        }
        std::size_t next = i + 1;
        while (next < text.size() && (static_cast<unsigned char>(text[next]) & 0xc0u) == 0x80u) ++next;
        const double advance = measurer.measure(text.substr(i, next - i), font);
        if (wrap && used + advance > max_width && i > start) {
            const auto end = soft > start ? soft : i;
            emit(end); start = end; i = end; soft = 0; used = 0;
            continue;
        }
        used += advance;
        if (text[i] == ' ' || text[i] == '\t') soft = next;
        i = next;
    }
    if (start < text.size()) emit(text.size());
    return lines;
}

}  // namespace prowsetk
