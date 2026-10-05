#ifndef PROWSETK_CORE_RENDER_CSS_SYNTAX_HPP
#define PROWSETK_CORE_RENDER_CSS_SYNTAX_HPP

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <prowsetk/render.hpp>

// css_syntax.hpp — syntax stratum of the graphical core.
//
// Value-only records shared by the style, layout and paint strata: what a CSS
// token sequence means once it has been read. Nothing here touches the DOM,
// resolves a length against a viewport, or computes geometry. The parser in
// css_parser.cpp produces these; the style resolver consumes them.

namespace prowsetk::detail {

// Small text helpers shared by the syntax and style strata. They live here
// because both need exactly the same tokenizing behaviour for CSS: lowercasing
// keywords, case-insensitive comparison, and splitting on top-level separators
// while ignoring separators inside strings, parentheses and brackets.
std::string lowered(std::string_view text);
bool iequals(std::string_view a, std::string_view b);
bool is_css_space(char c);
std::vector<std::string> split_top_level(std::string_view text, bool split_commas);

// One CSS component value list, already whitespace-separated at the top level.
struct ValueList {
    std::vector<std::string> tokens;

    bool empty() const noexcept { return tokens.empty(); }
    std::string_view first() const noexcept {
        return tokens.empty() ? std::string_view{} : std::string_view{tokens.front()};
    }
    // Case-insensitive keyword test against the first token.
    bool keyword_is(std::string_view keyword) const;
    // Number of tokens equal to `keyword`, ignoring case.
    std::size_t count(std::string_view keyword) const;
};

// A box shorthand: one to four lengths in CSS order (top, right, bottom, left).
struct BoxValues {
    // std::nullopt means the component was `auto`.
    std::optional<Length> top;
    std::optional<Length> right;
    std::optional<Length> bottom;
    std::optional<Length> left;

    // True when the component was absent rather than present-but-auto.
    bool present[4] = {false, false, false, false};

    std::optional<Length> at(std::size_t index) const;
    void set(std::size_t index, std::optional<Length> value);
};

// --- Individual property parsers --------------------------------------------
// Each returns std::nullopt for a value it does not recognize, so the resolver
// can ignore a declaration it cannot honour instead of failing the sheet.

std::optional<Display> parse_display(std::string_view value);
std::optional<Position> parse_position(std::string_view value);
std::optional<WhiteSpace> parse_white_space(std::string_view value);
std::optional<TextAlign> parse_text_align(std::string_view value);
std::optional<Overflow> parse_overflow(std::string_view value);
std::optional<Visibility> parse_visibility(std::string_view value);
std::optional<VerticalAlignHint> parse_vertical_align(std::string_view value);
std::optional<ObjectFit> parse_object_fit(std::string_view value);
std::optional<ListStyleType> parse_list_style_type(std::string_view value);
std::optional<ListStylePosition> parse_list_style_position(std::string_view value);
std::optional<BorderStyle> parse_border_style(std::string_view value);
std::optional<Color> parse_color(std::string_view value);
// A length keeps its unit unresolved; the style resolver converts it against the
// element's font size, the root font size or the viewport.
std::optional<Length> parse_length(std::string_view value);
std::optional<double> parse_number(std::string_view value);
std::optional<double> parse_opacity(std::string_view value);
std::optional<double> parse_font_weight(std::string_view value);
std::optional<double> parse_font_size(std::string_view value, double parent_size);
std::optional<std::string> parse_font_family(std::string_view value);
// Splits a `margin`/`padding` shorthand into up to four lengths. Returns nullopt
// when any component is not a length.
std::optional<BoxValues> parse_box(std::string_view value);
// Splits a `border`/`border-width`/`border-color`/`border-style` shorthand
// into its per-edge parts. Components absent from the shorthand are reported as
// present=false, which is distinct from a present-but-zero value. Any
// unrecognized token yields nullopt so one bad shorthand is skipped instead of
// discarding the rule.
struct BorderShorthand {
    double widths[4] = {0.0, 0.0, 0.0, 0.0};
    Color colors[4] = {};
    BorderStyle styles[4] = {BorderStyle::None, BorderStyle::None,
                              BorderStyle::None, BorderStyle::None};
    bool width_present[4] = {false, false, false, false};
    bool color_present[4] = {false, false, false, false};
    bool style_present[4] = {false, false, false, false};
};
std::optional<BorderShorthand> parse_border_shorthand(std::string_view value);
// Extracts the url() token of a `background-image`/`content` value. Returns
// std::nullopt for `none` or any value without a url(). The URL is returned
// exactly as written; resolution against the base URL is the resolver's job.
std::optional<std::string> parse_url_token(std::string_view value);
// Splits a comma-separated selector list, respecting parentheses, strings and
// brackets, so a selector containing a comma in an attribute value survives.
std::vector<std::string> split_selector_list(std::string_view selectors);
// Computes (id, class, type) specificity for one complex selector. Pseudo-
// classes other than the structural ones Flatworm understands count as a class.
void compute_specificity(std::string_view selector, CssRule& rule);

}  // namespace prowsetk::detail

#endif  // PROWSETK_CORE_RENDER_CSS_SYNTAX_HPP