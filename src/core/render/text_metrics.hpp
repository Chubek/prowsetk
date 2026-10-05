#ifndef PROWSETK_CORE_RENDER_TEXT_METRICS_HPP
#define PROWSETK_CORE_RENDER_TEXT_METRICS_HPP

#include <string>
#include <string_view>

#include <prowsetk/render.hpp>

// text_metrics.cpp — text stratum of the graphical core.
//
// Owns font measurement and line breaking. It sits between the style stratum
// (which resolves `font-size`, `line-height` and `white-space`) and the layout
// stratum (which needs to know where a run breaks), and it depends on nothing
// else in the renderer. Glyph rasterization is deliberately not here: a host
// paints the text runs the layout produced with its own toolkit.
//
// The default measurer approximates advances analytically rather than shaping
// text, so it needs no font file and works on a machine with no fonts installed.
// A host that draws with a real face supplies its own TextMeasurer so layout
// wraps exactly where the painted glyphs fit; that substitution is the whole
// point of the interface.

namespace prowsetk::detail {

// Deterministic fallback metrics. Not a substitute for a font: `family` is
// honoured only for a monospace family, which is the one case that can be
// answered exactly without a face.
class ApproximateTextMeasurer : public TextMeasurer {
public:
    FontMetrics metrics(const FontSpec& font) const override;
    double measure(std::string_view text, const FontSpec& font) const override;
};

// True when `family` names a monospace family, i.e. the generic `monospace`
// keyword or a family commonly available in that shape. Consumers use it to
// decide whether a fixed advance is honest.
bool is_monospace_family(std::string_view family);

// Line height for `font` under `line_height`, where a line_height of 0 means
// `normal`. A keyword or number is read as a multiple of the font size; a
// length is used as computed pixels.
double resolve_line_height(double font_size, double line_height,
                           const FontMetrics& metrics);

// One break opportunity found in a text run.
struct BreakOpportunity {
    // Offset of the break: the line keeps [start, offset) and the next starts
    // at `offset`.
    std::size_t offset = 0;
    // True when the break should consume following collapsible whitespace.
    bool collapsible = true;
};

// Finds break opportunities in `text` under `white_space`. Exposed for tests
// and for hosts laying out their own text; `break_text` is the normal entry.
std::vector<BreakOpportunity> find_breaks(std::string_view text,
                                          WhiteSpace white_space);

}  // namespace prowsetk::detail

#endif  // PROWSETK_CORE_RENDER_TEXT_METRICS_HPP