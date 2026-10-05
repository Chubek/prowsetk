#ifndef PROWSETK_CORE_RENDER_RENDER_INTERNAL_HPP
#define PROWSETK_CORE_RENDER_RENDER_INTERNAL_HPP

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <prowsetk/document.hpp>
#include <prowsetk/render.hpp>

// render_internal.hpp — contracts shared inside the graphical core only.
//
// Nothing here is a public API. Each function is defined by exactly one
// translation unit in this directory, and the boundaries are the ones documented
// in render.hpp: syntax feeds style, style feeds layout, layout feeds paint,
// and paint feeds hit testing. Keeping the intermediates here is what stops one
// stratum from reaching into another's decisions.

namespace prowsetk::detail {

// Environment the cascade needs to resolve relative units. Kept explicit so a
// render is a pure function of document plus these values.
struct StyleContext {
    double viewport_width = 1024.0;
    double viewport_height = 768.0;
    double root_font_size = 16.0;
};

// --- style_resolver.cpp ------------------------------------------------------

// The user-agent stylesheet for the tag set the subset models. Written as CSS so
// it goes through the same parser and cascade as author CSS: one code path, and
// a page cannot be defeated by having two stylesheet implementations.
std::string_view user_agent_stylesheet();

// Index of an element within the cascade's element vector, or npos.
constexpr std::size_t npos_index = static_cast<std::size_t>(-1);

// --- box_layout.cpp ----------------------------------------------------------

// One positioned piece of an element's border box. A block-level element
// produces one fragment; an inline element split across lines produces several.
struct BoxFragment {
    std::shared_ptr<Element> element;
    // Border box in document coordinates.
    Rect border_box;
    // Padding and border widths resolved to pixels, for painting.
    Insets padding;
    double border_widths[4] = {0.0, 0.0, 0.0, 0.0};
    Color background;
    BorderStyle border_style = BorderStyle::None;
    Color border_colors[4] = {};
    double opacity = 1.0;
    // Clip inherited from an ancestor with `overflow: hidden`.
    std::optional<Rect> clip;
    // Baseline of the first line, for vertical alignment inside an inline
    // context. Unused for block-level boxes.
    double baseline = 0.0;
    bool block_level = true;
    // A replaced element paints its own content instead of children.
    bool replaced = false;
};

// One already-wrapped line of text, positioned in document coordinates.
struct TextFragment {
    std::shared_ptr<Element> element;  // owning element, for identity
    Rect rect;                          // line box
    double baseline = 0.0;              // from the top of `rect`
    std::string text;
    FontSpec font;
    Color color{0, 0, 0, 255};
    TextAlign align = TextAlign::Start;
    bool underline = false;
    bool line_through = false;
    double opacity = 1.0;
    std::optional<Rect> clip;
};

// A replaced element's content box, ready for paint to decode and place.
struct ImageFragment {
    std::shared_ptr<Element> element;
    Rect rect;  // content box
    std::string source;  // resolved URL, or a data: URL
    std::string alt;
    ObjectFit object_fit = ObjectFit::Fill;
    double opacity = 1.0;
    std::optional<Rect> clip;
};

// A list marker box.
struct MarkerFragment {
    std::shared_ptr<Element> element;
    Rect rect;
    std::string text;
    FontSpec font;
    Color color{0, 0, 0, 255};
    double opacity = 1.0;
    std::optional<Rect> clip;
};

// A form control box. Paint turns this into a widget; layout never draws it.
struct ControlFragment {
    std::shared_ptr<Element> element;
    Rect rect;  // border box
    ControlKind kind = ControlKind::TextField;
    std::string value;
    std::string placeholder;
    std::vector<std::string> options;
    bool checked = false;
    bool disabled = false;
    bool focused = false;
    bool read_only = false;
    Color background{255, 255, 255, 255};
    Color border{118, 118, 118, 255};
    Color foreground{0, 0, 0, 255};
    double border_width = 1.0;
    FontSpec font;
    double opacity = 1.0;
    std::optional<Rect> clip;
};

// Everything the paint stratum consumes. Flat and ordered by document tree, so
// paint is a linear walk with no tree walking of its own.
struct LayoutResult {
    struct Entry { PaintKind kind; std::size_t index; };
    std::vector<Entry> order;
    std::vector<BoxFragment> boxes;
    std::vector<TextFragment> texts;
    std::vector<ImageFragment> images;
    std::vector<MarkerFragment> markers;
    std::vector<ControlFragment> controls;
    Rect bounds;
    bool limited = false;
};

// Lays out `document` with resolved `styles`. Throws Error(ResourceLimit) above
// `max_render_elements` or `max_render_depth`, and Error(InvalidArgument) for a
// viewport outside `max_render_viewport_dimension`.
LayoutResult layout_document(const Document& document, const ComputedStyles& styles,
                             const StyleContext& context, const TextMeasurer& measurer,
                             double pixel_ratio);

// --- paint.cpp ---------------------------------------------------------------

struct PaintContext {
    ImageCache* images = nullptr;
    ImageLoader* loader = nullptr;
    std::size_t max_images = max_render_images;
    bool paint_controls = true;
    std::size_t images_decoded = 0;
    bool limited = false;
};

// Lowers a layout to the display list. Never fetches anything itself: it asks
// `ctx.images` through `ctx.loader`, both of which the caller supplied.
PaintList paint_layout(const LayoutResult& layout, PaintContext& context);

// --- hit_test.cpp ------------------------------------------------------------

// True when the tag is one a click acts on by default. `summary` toggles its
// parent `details`, matching the engine's existing disclosure behaviour.
bool is_interactive_tag(std::string_view tag);

}  // namespace prowsetk::detail

#endif  // PROWSETK_CORE_RENDER_RENDER_INTERNAL_HPP
