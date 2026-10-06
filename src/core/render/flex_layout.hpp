#ifndef PROWSETK_CORE_RENDER_FLEX_LAYOUT_HPP
#define PROWSETK_CORE_RENDER_FLEX_LAYOUT_HPP

#include <prowsetk/render.hpp>

namespace prowsetk::detail {

// Value-only horizontal sizing. No DOM traversal, text measurement or painting.
// Widths are content-box widths; fixed contains padding, borders and margins.
struct FlexMeasure {
    double basis = 0, minimum = 0, maximum = max_render_content_pixels;
    double fixed = 0, grow = 0, shrink = 1;
    double margin_left = 0, margin_right = 0;
    bool auto_left = false, auto_right = false;
    double width = 0, x = 0;
};
struct FlexLine { std::size_t begin, end; };

std::vector<FlexLine> arrange_flex_rows(std::vector<FlexMeasure>& items,
    double width, double gap, FlexWrap wrap, FlexDirection direction,
    JustifyContent justify, std::size_t& work_remaining);

} // namespace prowsetk::detail
#endif
