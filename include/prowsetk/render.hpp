#ifndef PROWSETK_RENDER_HPP
#define PROWSETK_RENDER_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>

#include "prowsetk/document.hpp"

namespace prowsetk {

// Bounded layout and painting of a loaded document (README "Explicit headless
// display lists").
//
// Flatworm owns the graphical capabilities of the engine; it still wraps no
// third-party browser engine and remains headless by default. Nothing here
// opens a socket, needs a display server, or rasterizes pixels: the result is a
// display list in document coordinates that a host (the optional
// `plugins/basic-gui` FLTK frontend, a print backend, a test, or a native
// plugin) paints with its own toolkit.
//
// The implementation is stratified the same way as the IR emitters (see
// `ir.hpp`), and each concern lives in its own translation unit under
// `src/core/render/`:
//
// - **Syntax** (`css_parser.cpp`): parses stylesheet text into rules and
//   declarations. Touches no DOM and computes no geometry.
// - **Style** (`style_resolver.cpp`): the cascade. Merges the user-agent
//   stylesheet, author rules and inline styles by origin, importance and
//   specificity, then resolves inheritance into computed values. Reuses the
//   Flatworm selector engine; it never re-implements selector matching.
// - **Text** (`text_metrics.cpp`): font measurement and line breaking behind a
//   swappable `TextMeasurer`, so layout and the eventual glyph rasterizer agree
//   on advances.
// - **Layout** (`box_layout.cpp`): the box tree. Consumes computed styles and
//   produces positioned fragments. It never paints and never fetches.
// - **Paint** (`paint.cpp`): lowers positioned fragments to an ordered display
//   list in paint order.
// - **Images** (`image.cpp`): bounded decoding with `third_party/stb` behind a
//   host-mediated `ImageLoader`. The loader is always caller-supplied, so the
//   engine never opens a socket of its own.
// - **Hit testing** (`hit_test.cpp`): maps a document-space point back to the
//   element a host should interact with, honouring paint order and clipping.
//
// Layout support is deliberately a subset: block, inline, inline-block,
// list-item and replaced boxes (tables fall back to block flow); static and relative positioning; the
// margin/padding/border box model; backgrounds, borders, text, list markers,
// images and form controls. Flex, grid, floats, multi-column, transforms,
// stacking contexts, and text shaping beyond simple advance measurement are
// not implemented and are reported as Unsupported.

// --- Bounds ------------------------------------------------------------------
// Document, stylesheet and text/line limits throw ResourceLimit. Paint/image
// truncation yields a partial RenderedPage with limited set.

inline constexpr std::size_t max_render_stylesheet_bytes = 4u * 1024u * 1024u;
inline constexpr std::size_t max_render_rules = 40000;
inline constexpr std::size_t max_render_declarations = 40000;
inline constexpr std::size_t max_render_elements = 40000;
inline constexpr std::size_t max_render_depth = 256;
inline constexpr std::size_t max_render_paint_items = 400000;
inline constexpr double max_render_viewport_dimension = 16384.0;
inline constexpr double max_render_content_pixels = 4.0e7;
inline constexpr std::size_t max_render_images = 256;
inline constexpr std::size_t max_render_image_bytes = 8u * 1024u * 1024u;
inline constexpr std::size_t max_render_image_pixels = 16u * 1024u * 1024u;

// --- Geometry and colour ----------------------------------------------------

// Non-premultiplied sRGB. `a` is 0..255; 255 is fully opaque.
struct Color {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    std::uint8_t a = 255;

    constexpr bool operator==(const Color&) const noexcept = default;
};

// Axis-aligned rectangle in CSS pixels, document space. An empty rectangle has
// a non-positive width or height and contains nothing.
struct Rect {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;

    double right() const noexcept { return x + width; }
    double bottom() const noexcept { return y + height; }
    bool empty() const noexcept { return width <= 0.0 || height <= 0.0; }
    bool contains(double px, double py) const noexcept {
        return !empty() && px >= x && px < right() && py >= y && py < bottom();
    }
    Rect intersect(const Rect& other) const noexcept {
        const double l = x > other.x ? x : other.x;
        const double t = y > other.y ? y : other.y;
        const double r = right() < other.right() ? right() : other.right();
        const double b = bottom() < other.bottom() ? bottom() : other.bottom();
        if (r <= l || b <= t) return {};
        return {l, t, r - l, b - t};
    }
};

// Top/right/bottom/left lengths, used for padding, border and margin.
struct Insets {
    double top = 0.0;
    double right = 0.0;
    double bottom = 0.0;
    double left = 0.0;

    double horizontal() const noexcept { return left + right; }
    double vertical() const noexcept { return top + bottom; }
};

// --- Lengths -----------------------------------------------------------------

enum class LengthUnit {
    Auto,   // `auto`
    Px,
    Percent,
    Em,
    Rem,
    Vw,
    Vh,
    Vmin,
    Vmax,
};

// A CSS length. `is_auto` is carried explicitly so a resolved value of zero is
// distinguishable from `auto`.
struct Length {
    double value = 0.0;
    LengthUnit unit = LengthUnit::Px;
    bool is_auto = false;

    static Length auto_length() { return {0.0, LengthUnit::Auto, true}; }
    static Length px(double v) { return {v, LengthUnit::Px, false}; }
};

// --- Fonts and text measurement ---------------------------------------------

// Everything a consumer needs to select and size a face. `weight` is the
// numeric CSS weight (400 normal, 700 bold); family is the requested list.
struct FontSpec {
    std::string family;
    double size = 16.0;
    int weight = 400;
    bool italic = false;

    bool operator==(const FontSpec&) const noexcept = default;
};

// Metrics for one styled run. Advances are in CSS pixels and are the contract
// between layout and painting: a consumer that measures glyphs differently will
// wrap lines at different points.
struct FontMetrics {
    double ascent = 0.0;
    double descent = 0.0;
    double line_gap = 0.0;
    double line_height = 0.0;
    // Space advance is used as the fallback when a run measures empty.
    double space_advance = 0.0;
    double average_advance = 0.0;

    bool empty() const noexcept { return line_height <= 0.0; }
};

// Measures text for layout. The default implementation is self-contained and
// headless; a frontend that draws with a real face supplies its own so that
// layout wraps exactly where the painted glyphs will fit.
class TextMeasurer {
public:
    virtual ~TextMeasurer() = default;

    virtual FontMetrics metrics(const FontSpec& font) const = 0;
    // Advance width of `text` (UTF-8) when drawn in `font`. Never negative.
    virtual double measure(std::string_view text, const FontSpec& font) const = 0;
};

// The built-in measurer. It is deterministic and
// needs no font files, so the core renderer works on a machine with no fonts
// installed. It approximates advances rather than performing text shaping; see
// the layering notes above.
const TextMeasurer& default_text_measurer();

// --- Computed style ----------------------------------------------------------

enum class Display {
    None,
    Block,
    Inline,
    InlineBlock,
    ListItem,
    Table,
    TableRow,
    TableCell,
};

// A form control the paint stratum draws natively rather than as text.
enum class ControlKindHint {
    None,
    TextField,
    PasswordField,
    Checkbox,
    Radio,
    Button,
    Submit,
    Select,
    TextArea,
};

enum class Position { Static, Relative };

enum class WhiteSpace { Normal, Pre, NoWrap, PreWrap, PreLine };

enum class TextAlign { Start, End, Center, Justify };

enum class BorderStyle { None, Solid, Dashed, Dotted, Double, Groove, Ridge };

enum class Overflow { Visible, Hidden };

enum class Visibility { Visible, Hidden };

enum class ListStyleType { None, Disc, Circle, Square, Decimal, LowerAlpha, UpperAlpha,
                           LowerRoman, UpperRoman };

enum class ListStylePosition { Outside, Inside };

enum class ObjectFit { Fill, Contain, Cover, None, ScaleDown };

// Baseline/subject alignment for inline boxes. Only the values the inline
// formatting context needs are modelled.
enum class VerticalAlignHint { Baseline, Top, Middle, Bottom, TextTop, TextBottom };

// The resolved style of one element. Only properties the layout and paint
// strata consume are present alongside advisory hints for consumers. Unsupported
// properties are ignored; see README for the exact supported subset.
struct ComputedStyle {
    Display display = Display::Inline;
    Position position = Position::Static;
    // Relative offsets. Only `left` and `top` are honoured; `right`/`bottom`
    // are not, which is stated rather than approximated.
    Length offset_x;
    Length offset_y;

    Length width;
    Length height;
    Length min_width;
    Length min_height;
    Length max_width;
    Length max_height;

    Insets margin;
    Insets padding;
    double border_width[4] = {0.0, 0.0, 0.0, 0.0};  // top, right, bottom, left
    BorderStyle border_style = BorderStyle::None;
    Color border_color[4] = {};

    Color color{0, 0, 0, 255};
    Color background_color{0, 0, 0, 0};
    // Empty for `none`. Otherwise an absolute or document-relative URL exactly
    // as the style resolved it; decoding is the caller's via ImageLoader.
    std::string background_image;
    double opacity = 1.0;

    FontSpec font;
    double line_height = 0.0;  // 0 means `normal`
    TextAlign text_align = TextAlign::Start;
    bool underline = false;
    bool line_through = false;
    WhiteSpace white_space = WhiteSpace::Normal;
    VerticalAlignHint vertical_align = VerticalAlignHint::Baseline;
    Overflow overflow = Overflow::Visible;
    Visibility visibility = Visibility::Visible;

    ListStyleType list_style_type = ListStyleType::None;
    ListStylePosition list_style_position = ListStylePosition::Outside;

    ObjectFit object_fit = ObjectFit::Fill;
    // Document-relative image URL for replaced elements (`img`, `canvas`).
    std::string replaced_source;
    std::string replaced_alt;

    bool is_replaced = false;
    // Form-control semantics, resolved from the element and its `type`.
    ControlKindHint control = ControlKindHint::None;
    std::string control_type;
    std::string control_value;
    std::string control_placeholder;
    bool control_checked = false;
    bool control_disabled = false;
    bool control_read_only = false;
    std::vector<std::string> control_options;

    // Cursor hint, advisory: a host may map it onto its own pointer shape.
    std::string cursor;
};

// --- Style cascade -----------------------------------------------------------

struct Declaration {
    std::string property;
    std::string value;
    bool important = false;
};

// A parsed author or user-agent stylesheet.
struct CssRule {
    std::string selector;              // selector list as written
    std::vector<Declaration> declarations;
    // (id, class, tag) counts of the highest-weight compound; used for cascade
    // ordering without re-parsing during matching.
    unsigned specificity_id = 0;
    unsigned specificity_class = 0;
    unsigned specificity_type = 0;
    std::size_t order = 0;             // document order, the final tiebreaker
};

// Parses stylesheet text. At-rules are skipped except `@media screen`/`all`,
// whose contents are treated as unconditional; unknown at-rules, malformed
// blocks and unparsable declarations are skipped individually so one bad rule
// never discards a sheet. Throws Error(ResourceLimit) above
// `max_render_stylesheet_bytes`, `max_render_rules` or
// `max_render_declarations`.
std::vector<CssRule> parse_stylesheet(std::string_view css);

// The result of the cascade for one document: computed styles keyed by node
// identity, plus the user-agent defaults the subset implements.
class ComputedStyles {
public:
    // Style for an element, or nullptr when it was not part of the resolved
    // document.
    const ComputedStyle* find(const Element& element) const;
    const ComputedStyle& at(const Element& element) const;

    const ComputedStyle& root() const noexcept { return data_->root; }

    // Elements in document order, the same order `Document::query_selector_all`
    // returns, so index-based identity matches the inspection projection.
    const std::vector<std::shared_ptr<Element>>& elements() const noexcept {
        return data_->elements;
    }

    bool limited() const noexcept { return data_->limited; }

private:
    struct Data {
        std::unordered_map<const void*, std::size_t> index;
        std::vector<std::shared_ptr<Element>> elements;
        std::vector<ComputedStyle> styles;
        ComputedStyle root;
        bool limited = false;
    };
    explicit ComputedStyles(std::unique_ptr<Data> data) : data_(std::move(data)) {}
    std::unique_ptr<Data> data_;
    // The only producer. `resolve_computed_styles` builds the cascade and owns
    // this representation, so the cascade's internals cannot leak into the API.
    friend ComputedStyles resolve_computed_styles(const Document&, double, double,
                                                  double);
};

// Resolves the user-agent stylesheet, the document's `<style>` elements and
// inline `style` attributes into computed values. `viewport_width` and
// `viewport_height` resolve viewport-relative units; `root_font_size` resolves
// `rem`. Malformed author CSS is skipped by the parser rather than thrown, so a
// unsupported selectors are skipped. Resource-limit errors still propagate.
ComputedStyles resolve_computed_styles(const Document& document,
                                       double viewport_width = 1024.0,
                                       double viewport_height = 768.0,
                                       double root_font_size = 16.0);

// --- Images ------------------------------------------------------------------

// A decoded image. Pixels are non-premultiplied RGBA8, row-major, and owned by
// the bitmap; decoding happens in the engine with `third_party/stb`.
class ImageBitmap {
public:
    // Decodes PNG or JPEG bytes. Returns nullptr for an
    // unrecognised or truncated encoding, for an image exceeding
    // `max_render_image_pixels`, or when the engine was built without stb.
    // Never throws.
    static std::shared_ptr<const ImageBitmap> decode(
        std::span<const std::uint8_t> bytes);

    unsigned width() const noexcept { return width_; }
    unsigned height() const noexcept { return height_; }
    std::span<const std::uint8_t> rgba() const noexcept { return pixels_; }
    std::size_t byte_size() const noexcept { return pixels_.size(); }

private:
    ImageBitmap(unsigned width, unsigned height, std::vector<std::uint8_t> pixels)
        : width_(width), height_(height), pixels_(std::move(pixels)) {}
    unsigned width_;
    unsigned height_;
    std::vector<std::uint8_t> pixels_;
};

// Host-mediated image retrieval. The engine supplies no implementation: a host
// that allows network image loading passes one that applies its own session
// policy (cookies, redirects, TLS verification, response limits), and a host
// that does not passes nothing. Returning empty bytes means "unavailable"; the
// paint stratum then falls back to the element's alt text.
class ImageLoader {
public:
    virtual ~ImageLoader() = default;
    virtual std::vector<std::uint8_t> load(std::string_view absolute_url) = 0;
};

// Decodes and retains images across repaints. A caller owns the cache and may
// reuse it while the document is unchanged. The cache enforces
// `max_render_images`, `max_render_image_bytes` and `max_render_image_pixels`,
// dropping later requests rather than growing without bound.
class ImageCache {
public:
    ImageCache();
    ~ImageCache();
    ImageCache(const ImageCache&) = delete;
    ImageCache& operator=(const ImageCache&) = delete;

    // Returns the decoded image for `absolute_url`, or nullptr when it is
    // unavailable, unsupported, or over a budget. Repeated requests for the
    // same URL are served from the cache, including negative results.
    std::shared_ptr<const ImageBitmap> get(ImageLoader& loader,
                                          std::string_view absolute_url);

    void clear();
    std::size_t size() const noexcept;
    std::size_t byte_size() const noexcept;
    // Entries dropped because a budget was reached, for honest reporting.
    std::size_t dropped() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// --- Display list ------------------------------------------------------------

enum class PaintKind {
    Background,  // a filled box
    Border,      // up to four edges
    Text,        // one already-wrapped line of text
    Image,       // a replaced element's content, with an alt-text fallback
    Marker,      // a list marker box
    Control,     // a form control the consumer draws natively
};

// How a `Control` item should be presented.
enum class ControlKind {
    None,
    TextField,
    PasswordField,
    Checkbox,
    Radio,
    Button,
    Submit,
    Select,
    TextArea,
};

// One entry of the display list, in paint order: later items are painted over
// earlier ones. Rectangles are in document coordinates, so a host scrolls by
// translating them itself and hit tests against them directly.
//
// A single record with a discriminator follows the `ProwseEvent` precedent so a
// consumer iterates one vector and switches, rather than walking a variant.
struct PaintItem {
    PaintKind kind = PaintKind::Background;
    // Element that produced this item. Identity, not an index: it stays valid
    // while the document lives, and a host maps it back to the DOM itself.
    std::shared_ptr<Element> element;

    Rect rect;
    // Clipping rectangle from an ancestor's `overflow: hidden`, intersected
    // across the chain. Absent means unclipped.
    std::optional<Rect> clip;

    // Background and Border.
    Color color{0, 0, 0, 0};
    Insets padding;
    double border_width[4] = {0.0, 0.0, 0.0, 0.0};
    BorderStyle border_style = BorderStyle::Solid;
    double opacity = 1.0;

    // Text and Marker. `rect` is the line box; `baseline` is the distance from
    // the top of `rect` to the text baseline. `text` is already trimmed and
    // already broken for `rect.width`.
    std::string text;
    FontSpec font;
    double baseline = 0.0;
    TextAlign align = TextAlign::Start;

    // Image.
    std::shared_ptr<const ImageBitmap> bitmap;
    std::string alt;
    ObjectFit object_fit = ObjectFit::Fill;

    // Control.
    ControlKind control = ControlKind::None;
    bool checked = false;
    bool disabled = false;
    bool focused = false;
    bool read_only = false;
    std::string placeholder;

    // True when the item was produced but its content was unavailable (a
    // missing image, an unsupported property). Consumers should still paint
    // something meaningful.
    bool degraded = false;
};

struct PaintList {
    std::vector<PaintItem> items;
    // Union of painted extents, in document coordinates.
    Rect bounds;

    std::size_t size() const noexcept { return items.size(); }
    bool empty() const noexcept { return items.empty(); }
    void clear() noexcept { items.clear(); bounds = {}; }
};

// --- Hit testing -------------------------------------------------------------

enum class HitKind {
    None,      // nothing painted at the point
    Element,   // an ordinary element
    Interactive,  // a, button, input, select, textarea, summary, [onclick]
    Control,   // a form control
    Text,      // a text run, without its own interactive ancestor
};

// The element a host should act on for a point in document coordinates.
struct HitResult {
    HitKind kind = HitKind::None;
    // Deepest painted element at the point, or nullptr.
    std::shared_ptr<Element> element;
    // Nearest interactive ancestor-or-self of `element`, or nullptr. This is
    // what a click should target.
    std::shared_ptr<Element> interactive;
    Rect rect;
};

// Returns the topmost item at `(x, y)` in document coordinates. Items are
// examined in reverse paint order, so a later item wins, and `clip` is honoured.
// When the topmost hit is not itself interactive, `interactive` is the closest
// interactive ancestor or self.
HitResult hit_test(const PaintList& paint, double x, double y);

// True when clicking `element` should act on it: a hyperlink, a button, a form
// control, a `<summary>`, or an element carrying an `on*` handler.
bool is_interactive_element(const Element& element);

// --- Rendering ---------------------------------------------------------------

struct RenderOptions {
    double viewport_width = 1024.0;
    double viewport_height = 768.0;
    double pixel_ratio = 1.0;
    double root_font_size = 16.0;
    // Painting of form controls. When false, controls still occupy their layout
    // box but are painted as plain text fields.
    bool paint_controls = true;
    // Resolves `em`, `rem` and viewport units, and drives line breaking. When
    // null, `default_text_measurer()` is used.
    const TextMeasurer* measurer = nullptr;
    // Cache for decoded images. When null, no image is fetched or decoded and
    // every `img` degrades to its alt text, which keeps a render pure.
    ImageCache* images = nullptr;
    // Host-mediated retrieval for `images`. Ignored when `images` is null.
    ImageLoader* loader = nullptr;
    // Maximum `<img>` elements decoded during one render.
    std::size_t max_images = max_render_images;
};

struct RenderedPage {
    PaintList paint;
    // Scrolling bounds: the union of painted extents, never smaller than the
    // viewport.
    double content_width = 0.0;
    double content_height = 0.0;
    // Set when a bound was reached and painting is partial. The page remains
    // usable; callers should surface it rather than hide it.
    bool limited = false;
};

// Lays out and paints `document`. Deterministic for a fixed document,
// options and image results. It never opens a socket or logs. Loader errors
// propagate. Throws Error(InvalidArgument) for a viewport outside
// `max_render_viewport_dimension`, and Error(ResourceLimit) when the document
// itself exceeds `max_render_elements` or `max_render_depth`.
RenderedPage render_document(const Document& document,
                             const RenderOptions& options = {});

// Line breaking shared by the layout stratum and available to hosts that lay
// out their own text. Breaks `text` into lines no wider than `max_width`,
// honouring `white_space`, and returns the offsets into `text` each line spans.
std::vector<std::pair<std::size_t, std::size_t>> break_text(
    std::string_view text, const FontSpec& font, double max_width,
    WhiteSpace white_space, const TextMeasurer& measurer);

}  // namespace prowsetk

#endif  // PROWSETK_RENDER_HPP
