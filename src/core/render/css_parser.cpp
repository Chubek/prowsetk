// css_parser.cpp — syntax stratum: CSS text to records.
//
// Owns every decision about what a stylesheet or a declaration *says*. It
// reads text and produces rules, lengths, colors and keywords; it never sees a
// DOM node, resolves a unit against a viewport, or computes geometry. The style
// resolver consumes these records.
//
// Robustness is the design constraint: a hostile or merely broken page must not
// be able to fail a render. Malformed rules, unterminated blocks, unparsable
// declarations and unknown at-rules are skipped individually. Only the explicit
// byte/rule/declaration budgets raise Error(ResourceLimit), and they exist so a
// pathological sheet cannot consume unbounded memory or time.

#include "core/render/css_syntax.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>

#include <prowsetk/error.hpp>

namespace prowsetk::detail {
namespace {

bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

char lower(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool ends_with_ci(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() &&
           iequals(text.substr(text.size() - suffix.size()), suffix);
}

std::string_view trim(std::string_view text) {
    std::size_t begin = 0, end = text.size();
    while (begin < end && is_space(text[begin])) ++begin;
    while (end > begin && is_space(text[end - 1])) --end;
    return text.substr(begin, end - begin);
}

// --- numbers -----------------------------------------------------------------

// Strict numeric parse: accepts a leading sign, digits, an optional fraction
// and an optional exponent. Rejects `inf`, `nan` and trailing junk so a
// malformed value cannot poison a length with a non-finite number.
bool parse_number_strict(std::string_view text, double& out) {
    if (text.empty()) return false;
    std::size_t i = 0;
    if (text[i] == '+' || text[i] == '-') ++i;
    std::size_t digits = 0, dot = 0;
    while (i < text.size() && (std::isdigit(static_cast<unsigned char>(text[i])) != 0)) { ++i; ++digits; }
    if (i < text.size() && text[i] == '.') {
        ++i; ++dot;
        while (i < text.size() && (std::isdigit(static_cast<unsigned char>(text[i])) != 0)) { ++i; ++digits; }
    }
    if (digits == 0 || dot > 1) return false;
    if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
        ++i;
        if (i < text.size() && (text[i] == '+' || text[i] == '-')) ++i;
        std::size_t exp_digits = 0;
        while (i < text.size() && (std::isdigit(static_cast<unsigned char>(text[i])) != 0)) { ++i; ++exp_digits; }
        if (exp_digits == 0) return false;
    }
    if (i != text.size()) return false;
    // from_chars accepts a leading '-' only; normalize '+' and the fraction.
    std::string normalized(text);
    if (!normalized.empty() && normalized.front() == '+') normalized.erase(normalized.begin());
    if (!normalized.empty() && normalized.front() == '.') normalized.insert(normalized.begin(), '0');
    const auto dot_it = normalized.find('.');
    if (dot_it != std::string::npos && dot_it + 1 == normalized.size()) normalized.push_back('0');
    const auto result = std::from_chars(normalized.data(),
                                       normalized.data() + normalized.size(), out);
    return result.ec == std::errc{} && result.ptr == normalized.data() + normalized.size() &&
           std::isfinite(out);
}

// --- colors ------------------------------------------------------------------

struct NamedColor {
    std::string_view name;
    std::uint32_t rgb;
};

// The CSS basic and extended colour keywords the paint stratum needs. Kept
// explicit rather than generated: the set is small, and a table is auditable.
constexpr std::array<NamedColor, 148> kNamedColors{{
    {"aliceblue", 0xf0f8ff}, {"antiquewhite", 0xfaebd7}, {"aqua", 0x00ffff},
    {"aquamarine", 0x7fffd4}, {"azure", 0xf0ffff}, {"beige", 0xf5f5dc},
    {"bisque", 0xffe4c4}, {"black", 0x000000}, {"blanchedalmond", 0xffebcd},
    {"blue", 0x0000ff}, {"blueviolet", 0x8a2be2}, {"brown", 0xa52a2a},
    {"burlywood", 0xdeb887}, {"cadetblue", 0x5f9ea0}, {"chartreuse", 0x7fff00},
    {"chocolate", 0xd2691e}, {"coral", 0xff7f50}, {"cornflowerblue", 0x6495ed},
    {"cornsilk", 0xfff8dc}, {"crimson", 0xdc143c}, {"cyan", 0x00ffff},
    {"darkblue", 0x00008b}, {"darkcyan", 0x008b8b}, {"darkgoldenrod", 0xb8860b},
    {"darkgray", 0xa9a9a9}, {"darkgreen", 0x006400}, {"darkgrey", 0xa9a9a9},
    {"darkkhaki", 0xbdb76b}, {"darkmagenta", 0x8b008b}, {"darkolivegreen", 0x556b2f},
    {"darkorange", 0xff8c00}, {"darkorchid", 0x9932cc}, {"darkred", 0x8b0000},
    {"darksalmon", 0xe9967a}, {"darkseagreen", 0x8fbc8f}, {"darkslateblue", 0x483d8b},
    {"darkslategray", 0x2f4f4f}, {"darkslategrey", 0x2f4f4f}, {"darkturquoise", 0x00ced1},
    {"darkviolet", 0x9400d3}, {"deeppink", 0xff1493}, {"deepskyblue", 0x00bfff},
    {"dimgray", 0x696969}, {"dimgrey", 0x696969}, {"dodgerblue", 0x1e90ff},
    {"firebrick", 0xb22222}, {"floralwhite", 0xfffaf0}, {"forestgreen", 0x228b22},
    {"fuchsia", 0xff00ff}, {"gainsboro", 0xdcdcdc}, {"ghostwhite", 0xf8f8ff},
    {"gold", 0xffd700}, {"goldenrod", 0xdaa520}, {"gray", 0x808080},
    {"green", 0x008000}, {"greenyellow", 0xadff2f}, {"grey", 0x808080},
    {"honeydew", 0xf0fff0}, {"hotpink", 0xff69b4}, {"indianred", 0xcd5c5c},
    {"indigo", 0x4b0082}, {"ivory", 0xfffff0}, {"khaki", 0xf0e68c},
    {"lavender", 0xe6e6fa}, {"lavenderblush", 0xfff0f5}, {"lawngreen", 0x7cfc00},
    {"lemonchiffon", 0xfffacd}, {"lightblue", 0xadd8e6}, {"lightcoral", 0xf08080},
    {"lightcyan", 0xe0ffff}, {"lightgoldenrodyellow", 0xfafad2}, {"lightgray", 0xd3d3d3},
    {"lightgreen", 0x90ee90}, {"lightgrey", 0xd3d3d3}, {"lightpink", 0xffb6c1},
    {"lightsalmon", 0xffa07a}, {"lightseagreen", 0x20b2aa}, {"lightskyblue", 0x87cefa},
    {"lightslategray", 0x778899}, {"lightslategrey", 0x778899}, {"lightsteelblue", 0xb0c4de},
    {"lightyellow", 0xffffe0}, {"lime", 0x00ff00}, {"limegreen", 0x32cd32},
    {"linen", 0xfaf0e6}, {"magenta", 0xff00ff}, {"maroon", 0x800000},
    {"mediumaquamarine", 0x66cdaa}, {"mediumblue", 0x0000cd}, {"mediumorchid", 0xba55d3},
    {"mediumpurple", 0x9370db}, {"mediumseagreen", 0x3cb371}, {"mediumslateblue", 0x7b68ee},
    {"mediumspringgreen", 0x00fa9a}, {"mediumturquoise", 0x48d1cc}, {"mediumvioletred", 0xc71585},
    {"midnightblue", 0x191970}, {"mintcream", 0xf5fffa}, {"mistyrose", 0xffe4e1},
    {"moccasin", 0xffe4b5}, {"navajowhite", 0xffdead}, {"navy", 0x000080},
    {"oldlace", 0xfdf5e6}, {"olive", 0x808000}, {"olivedrab", 0x6b8e23},
    {"orange", 0xffa500}, {"orangered", 0xff4500}, {"orchid", 0xda70d6},
    {"palegoldenrod", 0xeee8aa}, {"palegreen", 0x98fb98}, {"paleturquoise", 0xafeeee},
    {"palevioletred", 0xdb7093}, {"papayawhip", 0xffefd5}, {"peachpuff", 0xffdab9},
    {"peru", 0xcd853f}, {"pink", 0xffc0cb}, {"plum", 0xdda0dd},
    {"powderblue", 0xb0e0e6}, {"purple", 0x800080}, {"rebeccapurple", 0x663399},
    {"red", 0xff0000}, {"rosybrown", 0xbc8f8f}, {"royalblue", 0x4169e1},
    {"saddlebrown", 0x8b4513}, {"salmon", 0xfa8072}, {"sandybrown", 0xf4a460},
    {"seagreen", 0x2e8b57}, {"seashell", 0xfff5ee}, {"sienna", 0xa0522d},
    {"silver", 0xc0c0c0}, {"skyblue", 0x87ceeb}, {"slateblue", 0x6a5acd},
    {"slategray", 0x708090}, {"slategrey", 0x708090}, {"snow", 0xfffafa},
    {"springgreen", 0x00ff7f}, {"steelblue", 0x4682b4}, {"tan", 0xd2b48c},
    {"teal", 0x008080}, {"thistle", 0xd8bfd8}, {"tomato", 0xff6347},
    {"turquoise", 0x40e0d0}, {"violet", 0xee82ee}, {"wheat", 0xf5deb3},
    {"white", 0xffffff}, {"whitesmoke", 0xf5f5f5}, {"yellow", 0xffff00},
    {"yellowgreen", 0x9acd32},
}};

std::optional<std::uint32_t> hex_digits(std::string_view text) {
    if (text.empty() || text.size() > 8) return std::nullopt;
    std::uint32_t value = 0;
    for (const char c : text) {
        int digit = 0;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else return std::nullopt;
        value = (value << 4) | static_cast<std::uint32_t>(digit);
    }
    return value;
}

double clamp_unit(double value) {
    if (!(value > 0.0)) return 0.0;
    if (value > 1.0) return 1.0;
    return value;
}

std::uint8_t to_channel(double unit) {
    const double scaled = std::lround(clamp_unit(unit) * 255.0);
    return static_cast<std::uint8_t>(scaled);
}

double hue_to_channel(double hue) {
    hue = std::fmod(hue, 360.0);
    if (hue < 0.0) hue += 360.0;
    constexpr double c = 1.0;
    const double x = 1.0 - std::abs(std::fmod(hue / 60.0, 2.0) - 1.0);
    double rgb[3];
    if (hue < 60.0) { rgb[0] = c; rgb[1] = x; rgb[2] = 0.0; }
    else if (hue < 120.0) { rgb[0] = x; rgb[1] = c; rgb[2] = 0.0; }
    else if (hue < 180.0) { rgb[0] = 0.0; rgb[1] = c; rgb[2] = x; }
    else if (hue < 240.0) { rgb[0] = 0.0; rgb[1] = x; rgb[2] = c; }
    else if (hue < 300.0) { rgb[0] = x; rgb[1] = 0.0; rgb[2] = c; }
    else { rgb[0] = c; rgb[1] = 0.0; rgb[2] = x; }
    return rgb[0];
}

// Parses the argument list of a functional notation, accepting both the
// legacy comma form and the CSS Color 4 space form. Returns nullopt when the
// argument count or types are not understood.
std::optional<Color> parse_functional_color(std::string_view name,
                                            std::string_view args) {
    const bool rgb = name == "rgb" || name == "rgba";
    const bool hsl = name == "hsl" || name == "hsla";
    if (!rgb && !hsl) return std::nullopt;
    // Split on commas and spaces, dropping a single '/' separator.
    std::vector<std::string_view> parts;
    {
        std::string current;
        int depth = 0;
        for (const char c : args) {
            if (c == '(' || c == '[') ++depth;
            if (c == ')' || c == ']') --depth;
            if (depth == 0 && (is_space(c) || c == ',' || c == '/')) {
                if (!current.empty()) { parts.push_back(current); current.clear(); }
                continue;
            }
            current.push_back(c);
        }
        if (!current.empty()) parts.push_back(current);
    }
    if (parts.size() < 3 || parts.size() > 4) return std::nullopt;
    double alpha = 1.0;
    if (parts.size() == 4) {
        if (ends_with_ci(parts[3], "%")) {
            double pct = 0.0;
            if (!parse_number_strict(parts[3].substr(0, parts[3].size() - 1), pct)) return std::nullopt;
            alpha = pct / 100.0;
        } else if (!parse_number_strict(parts[3], alpha)) return std::nullopt;
    }
    // rgb() accepts 0..255 numbers or percentages; hsl() needs an angle.
    auto channel = [&](std::string_view text, bool percentage_scaled) -> std::optional<double> {
        if (ends_with_ci(text, "%")) {
            double pct = 0.0;
            if (!parse_number_strict(text.substr(0, text.size() - 1), pct)) return std::nullopt;
            return percentage_scaled ? pct / 100.0 : pct * 255.0 / 100.0;
        }
        double value = 0.0;
        if (!parse_number_strict(text, value)) return std::nullopt;
        return percentage_scaled ? value / 255.0 : value;
    };
    if (rgb) {
        const auto r = channel(parts[0], false);
        const auto g = channel(parts[1], false);
        const auto b = channel(parts[2], false);
        if (!r || !g || !b) return std::nullopt;
        return Color{to_channel(*r), to_channel(*g), to_channel(*b), to_channel(alpha)};
    }
    double hue = 0.0;
    if (ends_with_ci(parts[0], "deg")) {
        if (!parse_number_strict(parts[0].substr(0, parts[0].size() - 3), hue)) return std::nullopt;
    } else if (!parse_number_strict(parts[0], hue)) {
        return std::nullopt;
    }
    if (!parts[1].empty() && parts[1].back() == '%') {
        if (!parse_number_strict(parts[1].substr(0, parts[1].size() - 1), hue)) return std::nullopt;
    }
    const auto s = channel(parts[1], true);
    const auto l = channel(parts[2], true);
    if (!s || !l) return std::nullopt;
    return Color{to_channel(hue_to_channel(hue + 120.0)),
                 to_channel(hue_to_channel(hue)),
                 to_channel(hue_to_channel(hue - 120.0)),
                 to_channel(alpha)};
}

// Matches `name(` at the start of `text` and returns the callee plus arguments.
bool open_function(std::string_view text, std::string_view& name,
                   std::string_view& args) {
    const auto open = text.find('(');
    if (open == std::string_view::npos || open == 0 || text.back() != ')') {
        return false;
    }
    name = trim(text.substr(0, open));
    args = trim(text.substr(open + 1, text.size() - open - 2));
    return !name.empty();
}

}  // namespace

// --- shared text helpers -----------------------------------------------------
// Defined here (the syntax stratum) rather than duplicated, so the cascade and
// this file tokenize CSS identically.

std::string lowered(std::string_view text) {
    std::string out(text);
    for (auto& c : out) c = lower(c);
    return out;
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (lower(a[i]) != lower(b[i])) return false;
    }
    return true;
}

bool is_css_space(char c) { return is_space(c); }

// Splits on top-level whitespace and, when asked, on commas and `/`. Separators
// inside parentheses, brackets and strings are kept, so `rgb(0, 0, 0)` and
// `font: 12px/1.5 serif` survive as single tokens.
std::vector<std::string> split_top_level(std::string_view text, bool split_commas) {
    std::vector<std::string> parts;
    std::string current;
    int depth = 0;
    char quote = 0;
    const auto flush = [&] {
        if (!current.empty()) { parts.push_back(current); current.clear(); }
    };
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (quote != 0) {
            current.push_back(c);
            if (c == '\\' && i + 1 < text.size()) current.push_back(text[++i]);
            else if (c == quote) quote = 0;
            continue;
        }
        if (c == '"' || c == '\'') { quote = c; current.push_back(c); continue; }
        if (c == '(' || c == '[') { ++depth; current.push_back(c); continue; }
        if (c == ')' || c == ']') { if (depth > 0) --depth; current.push_back(c); continue; }
        if (depth == 0 && is_space(c)) { flush(); continue; }
        if (depth == 0 && split_commas && (c == ',' || c == '/')) {
            flush();
            parts.emplace_back(1, c);
            continue;
        }
        current.push_back(c);
    }
    flush();
    return parts;
}

// --- ValueList ---------------------------------------------------------------

bool ValueList::keyword_is(std::string_view keyword) const {
    return tokens.size() == 1 && iequals(tokens.front(), keyword);
}

std::size_t ValueList::count(std::string_view keyword) const {
    std::size_t total = 0;
    for (const auto& token : tokens) {
        if (iequals(token, keyword)) ++total;
    }
    return total;
}

// --- BoxValues ---------------------------------------------------------------

std::optional<Length> BoxValues::at(std::size_t index) const {
    switch (index) {
        case 0: return top;
        case 1: return right;
        case 2: return bottom;
        default: return left;
    }
}

void BoxValues::set(std::size_t index, std::optional<Length> value) {
    switch (index) {
        case 0: top = value; break;
        case 1: right = value; break;
        case 2: bottom = value; break;
        default: left = value; break;
    }
}

// --- keywords ----------------------------------------------------------------

std::optional<Display> parse_display(std::string_view value) {
    if (iequals(value, "none")) return Display::None;
    if (iequals(value, "block")) return Display::Block;
    if (iequals(value, "inline")) return Display::Inline;
    if (iequals(value, "inline-block")) return Display::InlineBlock;
    if (iequals(value, "flex")) return Display::Flex;
    if (iequals(value, "list-item")) return Display::ListItem;
    if (iequals(value, "table")) return Display::Table;
    if (iequals(value, "inline-table")) return Display::Table;
    if (iequals(value, "table-row")) return Display::TableRow;
    if (iequals(value, "inline-table-row")) return Display::TableRow;
    if (iequals(value, "table-cell")) return Display::TableCell;
    if (iequals(value, "inline-table-cell")) return Display::TableCell;
    // Unsupported formatting contexts keep a readable block fallback.
    if (iequals(value, "grid") || iequals(value, "inline-grid") ||
        iequals(value, "inline-flex")) {
        return Display::Block;
    }
    return std::nullopt;
}

std::optional<Position> parse_position(std::string_view value) {
    if (iequals(value, "static")) return Position::Static;
    if (iequals(value, "relative")) return Position::Relative;
    return std::nullopt;
}

std::optional<WhiteSpace> parse_white_space(std::string_view value) {
    if (iequals(value, "normal")) return WhiteSpace::Normal;
    if (iequals(value, "pre")) return WhiteSpace::Pre;
    if (iequals(value, "nowrap")) return WhiteSpace::NoWrap;
    if (iequals(value, "pre-wrap")) return WhiteSpace::PreWrap;
    if (iequals(value, "pre-line")) return WhiteSpace::PreLine;
    return std::nullopt;
}

std::optional<TextAlign> parse_text_align(std::string_view value) {
    if (iequals(value, "left") || iequals(value, "start")) return TextAlign::Start;
    if (iequals(value, "right") || iequals(value, "end")) return TextAlign::End;
    if (iequals(value, "center")) return TextAlign::Center;
    if (iequals(value, "justify")) return TextAlign::Justify;
    return std::nullopt;
}

std::optional<Overflow> parse_overflow(std::string_view value) {
    if (iequals(value, "visible")) return Overflow::Visible;
    if (iequals(value, "hidden")) return Overflow::Hidden;
    return std::nullopt;
}

std::optional<AlignItems> parse_align_items(std::string_view value) {
    if (iequals(value, "stretch")) return AlignItems::Stretch;
    if (iequals(value, "start") || iequals(value, "flex-start")) return AlignItems::Start;
    if (iequals(value, "end") || iequals(value, "flex-end")) return AlignItems::End;
    if (iequals(value, "center")) return AlignItems::Center;
    return std::nullopt;
}

std::optional<JustifyContent> parse_justify_content(std::string_view value) {
    if (iequals(value, "start") || iequals(value, "flex-start")) return JustifyContent::Start;
    if (iequals(value, "end") || iequals(value, "flex-end")) return JustifyContent::End;
    if (iequals(value, "center")) return JustifyContent::Center;
    if (iequals(value, "space-between")) return JustifyContent::SpaceBetween;
    if (iequals(value, "space-around")) return JustifyContent::SpaceAround;
    if (iequals(value, "space-evenly")) return JustifyContent::SpaceEvenly;
    return std::nullopt;
}

std::optional<FlexValues> parse_flex(std::string_view value) {
    if (iequals(value, "none")) return FlexValues{0, 0, Length::auto_length()};
    if (iequals(value, "auto")) return FlexValues{1, 1, Length::auto_length()};
    if (iequals(value, "initial")) return FlexValues{0, 1, Length::auto_length()};
    const auto tokens = split_top_level(value, true);
    if (tokens.empty() || tokens.size() > 3) return std::nullopt;
    FlexValues result;
    std::size_t numbers = 0;
    bool basis = false;
    for (const auto& token : tokens) {
        if (const auto number = parse_number(token); number && numbers < 2 && !basis) {
            if (*number < 0 || *number > 1.0e6) return std::nullopt;
            if (numbers++ == 0) result.grow = *number; else result.shrink = *number;
        } else if (const auto parsed = parse_length(token); parsed && !basis && parsed->value >= 0) {
            result.basis = *parsed;
            basis = true;
        } else return std::nullopt;
    }
    return result;
}

std::optional<Visibility> parse_visibility(std::string_view value) {
    if (iequals(value, "visible")) return Visibility::Visible;
    if (iequals(value, "hidden")) return Visibility::Hidden;
    // `collapse` behaves as hidden in the single formatting context modelled.
    if (iequals(value, "collapse")) return Visibility::Hidden;
    return std::nullopt;
}

std::optional<VerticalAlignHint> parse_vertical_align(std::string_view value) {
    if (iequals(value, "baseline")) return VerticalAlignHint::Baseline;
    if (iequals(value, "top")) return VerticalAlignHint::Top;
    if (iequals(value, "middle")) return VerticalAlignHint::Middle;
    if (iequals(value, "bottom")) return VerticalAlignHint::Bottom;
    if (iequals(value, "text-top")) return VerticalAlignHint::TextTop;
    if (iequals(value, "text-bottom")) return VerticalAlignHint::TextBottom;
    return std::nullopt;
}

std::optional<ObjectFit> parse_object_fit(std::string_view value) {
    if (iequals(value, "fill")) return ObjectFit::Fill;
    if (iequals(value, "contain")) return ObjectFit::Contain;
    if (iequals(value, "cover")) return ObjectFit::Cover;
    if (iequals(value, "none")) return ObjectFit::None;
    if (iequals(value, "scale-down")) return ObjectFit::ScaleDown;
    return std::nullopt;
}

std::optional<ListStyleType> parse_list_style_type(std::string_view value) {
    if (iequals(value, "none")) return ListStyleType::None;
    if (iequals(value, "disc")) return ListStyleType::Disc;
    if (iequals(value, "circle")) return ListStyleType::Circle;
    if (iequals(value, "square")) return ListStyleType::Square;
    if (iequals(value, "decimal")) return ListStyleType::Decimal;
    if (iequals(value, "lower-alpha") || iequals(value, "lower-latin")) {
        return ListStyleType::LowerAlpha;
    }
    if (iequals(value, "upper-alpha") || iequals(value, "upper-latin")) {
        return ListStyleType::UpperAlpha;
    }
    if (iequals(value, "lower-roman")) return ListStyleType::LowerRoman;
    if (iequals(value, "upper-roman")) return ListStyleType::UpperRoman;
    return std::nullopt;
}

std::optional<ListStylePosition> parse_list_style_position(std::string_view value) {
    if (iequals(value, "outside")) return ListStylePosition::Outside;
    if (iequals(value, "inside")) return ListStylePosition::Inside;
    return std::nullopt;
}

std::optional<BorderStyle> parse_border_style(std::string_view value) {
    if (iequals(value, "none")) return BorderStyle::None;
    if (iequals(value, "hidden")) return BorderStyle::None;
    if (iequals(value, "solid")) return BorderStyle::Solid;
    if (iequals(value, "dashed")) return BorderStyle::Dashed;
    if (iequals(value, "dotted")) return BorderStyle::Dotted;
    if (iequals(value, "double")) return BorderStyle::Double;
    if (iequals(value, "groove")) return BorderStyle::Groove;
    if (iequals(value, "ridge")) return BorderStyle::Ridge;
    return std::nullopt;
}

// --- colors and numbers ------------------------------------------------------

std::optional<Color> parse_color(std::string_view value) {
    const auto text = trim(value);
    if (text.empty()) return std::nullopt;
    if (iequals(text, "transparent")) return Color{0, 0, 0, 0};
    if (iequals(text, "currentcolor") || iequals(text, "inherit") ||
        iequals(text, "initial") || iequals(text, "unset")) {
        return std::nullopt;  // resolved by the cascade, not by the syntax
    }
    if (text.front() == '#') {
        const auto digits = hex_digits(text.substr(1));
        if (!digits) return std::nullopt;
        const auto raw = *digits;
        switch (text.size() - 1) {
            case 3: {
                const auto expand = [](std::uint32_t nibble) {
                    return static_cast<std::uint8_t>((nibble << 4) | nibble);
                };
                return Color{expand((raw >> 8) & 0xf), expand((raw >> 4) & 0xf),
                             expand(raw & 0xf), 255};
            }
            case 4: {
                const auto expand = [](std::uint32_t nibble) {
                    return static_cast<std::uint8_t>((nibble << 4) | nibble);
                };
                return Color{expand((raw >> 12) & 0xf), expand((raw >> 8) & 0xf),
                             expand((raw >> 4) & 0xf), expand(raw & 0xf)};
            }
            case 6:
                return Color{static_cast<std::uint8_t>((raw >> 16) & 0xff),
                             static_cast<std::uint8_t>((raw >> 8) & 0xff),
                             static_cast<std::uint8_t>(raw & 0xff), 255};
            case 8:
                return Color{static_cast<std::uint8_t>((raw >> 24) & 0xff),
                             static_cast<std::uint8_t>((raw >> 16) & 0xff),
                             static_cast<std::uint8_t>((raw >> 8) & 0xff),
                             static_cast<std::uint8_t>(raw & 0xff)};
            default: return std::nullopt;
        }
    }
    std::string_view name, args;
    if (open_function(text, name, args)) {
        const auto lower_name = lowered(name);
        if (auto color = parse_functional_color(lower_name, args)) return color;
        return std::nullopt;
    }
    const auto key = lowered(text);
    const auto found = std::lower_bound(
        kNamedColors.begin(), kNamedColors.end(), key,
        [](const NamedColor& entry, const std::string& needle) {
            return entry.name < std::string_view(needle);
        });
    if (found == kNamedColors.end() || found->name != std::string_view(key)) {
        return std::nullopt;
    }
    return Color{static_cast<std::uint8_t>((found->rgb >> 16) & 0xff),
                 static_cast<std::uint8_t>((found->rgb >> 8) & 0xff),
                 static_cast<std::uint8_t>(found->rgb & 0xff), 255};
}

std::optional<Length> parse_length(std::string_view value) {
    const auto text = trim(value);
    if (text.empty()) return std::nullopt;
    if (iequals(text, "auto")) return Length::auto_length();
    static constexpr std::array<std::pair<std::string_view, LengthUnit>, 8> kUnits{{
        {"px", LengthUnit::Px}, {"rem", LengthUnit::Rem}, {"em", LengthUnit::Em},
        {"vw", LengthUnit::Vw}, {"vh", LengthUnit::Vh}, {"vmin", LengthUnit::Vmin},
        {"vmax", LengthUnit::Vmax}, {"%", LengthUnit::Percent},
    }};
    for (const auto& [suffix, unit] : kUnits) {
        if (ends_with_ci(text, suffix)) {
            double number = 0.0;
            if (!parse_number_strict(text.substr(0, text.size() - suffix.size()), number)) {
                return std::nullopt;
            }
            // Guard against a hostile sheet expressing absurd lengths.
            if (std::abs(number) > 1.0e7) return std::nullopt;
            return Length{number, unit, false};
        }
    }
    // A bare number is a zero-unit length, as CSS requires.
    double number = 0.0;
    if (parse_number_strict(text, number) && std::abs(number) <= 1.0e7) {
        return Length{number, LengthUnit::Px, false};
    }
    return std::nullopt;
}

std::optional<double> parse_number(std::string_view value) {
    double number = 0.0;
    if (!parse_number_strict(trim(value), number)) return std::nullopt;
    return number;
}

std::optional<double> parse_opacity(std::string_view value) {
    const auto number = parse_number(value);
    if (!number) return std::nullopt;
    return clamp_unit(*number);
}

std::optional<double> parse_font_weight(std::string_view value) {
    if (iequals(trim(value), "normal")) return 400.0;
    if (iequals(trim(value), "bold")) return 700.0;
    if (iequals(trim(value), "bolder")) return 700.0;
    if (iequals(trim(value), "lighter")) return 300.0;
    const auto number = parse_number(value);
    if (number && *number >= 1.0 && *number <= 1000.0) return *number;
    return std::nullopt;
}

std::optional<double> parse_font_size(std::string_view value, double parent_size) {
    static constexpr std::array<std::pair<std::string_view, double>, 7> kAbsolute{{
        {"xx-small", 9.0}, {"x-small", 10.0}, {"small", 13.0}, {"medium", 16.0},
        {"large", 18.0}, {"x-large", 24.0}, {"xx-large", 32.0},
    }};
    const auto text = trim(value);
    const auto key = lowered(text);
    for (const auto& [name, size] : kAbsolute) {
        if (key == name) return size;
    }
    if (iequals(text, "smaller")) return parent_size / 1.2;
    if (iequals(text, "larger")) return parent_size * 1.2;
    const auto length = parse_length(text);
    if (!length) return std::nullopt;
    switch (length->unit) {
        case LengthUnit::Px: return length->value;
        case LengthUnit::Em: return parent_size * length->value;
        case LengthUnit::Rem: return length->value;
        case LengthUnit::Percent: return parent_size * length->value / 100.0;
        default: return std::nullopt;
    }
}

std::optional<std::string> parse_font_family(std::string_view value) {
    // Keep the first available family, unquoted and whitespace-collapsed. The
    // paint stratum decides whether it can honour it.
    const auto text = trim(value);
    if (text.empty()) return std::nullopt;
    std::size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && (is_space(text[i]) || text[i] == ',')) ++i;
        if (i >= text.size()) break;
        std::string family;
        if (text[i] == '"' || text[i] == '\'') {
            const char quote = text[i++];
            while (i < text.size() && text[i] != quote) {
                if (text[i] == '\\' && i + 1 < text.size()) ++i;
                family.push_back(text[i++]);
            }
            if (i < text.size()) ++i;  // closing quote
        } else {
            while (i < text.size() && !is_space(text[i]) && text[i] != ',') {
                if (text[i] == '\\' && i + 1 < text.size()) ++i;
                family.push_back(text[i++]);
            }
        }
        if (!family.empty()) {
            std::string collapsed;
            for (const char c : family) {
                if (!collapsed.empty() && is_space(c)) continue;
                collapsed.push_back(c);
            }
            return collapsed;
        }
    }
    return std::nullopt;
}

// --- shorthands --------------------------------------------------------------

namespace {
// Applies one to four `margin`/`padding`/`border-width` components in CSS order:
// one value fills all edges, two are top/bottom and left/right, three are
// top, left/right, bottom, four are explicit.
template <typename T, typename Parse>
std::optional<BoxValues> expand_box(std::string_view text, Parse parse_one) {
    const auto parts = split_top_level(trim(text), false);
    std::vector<std::optional<Length>> values;
    for (const auto& part : parts) {
        if (part == "/") return std::nullopt;
        values.push_back(parse_one(part));
        if (!values.back()) return std::nullopt;
    }
    if (values.empty() || values.size() > 4) return std::nullopt;
    BoxValues box;
    const std::size_t count = values.size();
    auto slot = [&](std::size_t index) -> std::optional<Length> {
        switch (count) {
            case 1: return values[0];
            case 2: return (index == 0 || index == 2) ? values[0] : values[1];
            case 3: return index == 1 ? values[1] : (index == 3 ? values[1] : values[index]);
            default: return values[index];
        }
    };
    for (std::size_t edge = 0; edge < 4; ++edge) {
        box.present[edge] = true;
        box.set(edge, slot(edge));
    }
    return box;
}
}  // namespace

std::optional<BoxValues> parse_box(std::string_view value) {
    return expand_box<Length>(value, [](const std::string& part) {
        return parse_length(part);
    });
}

std::optional<BorderShorthand> parse_border_shorthand(std::string_view value) {
    const auto parts = split_top_level(trim(value), false);
    BorderShorthand result;
    std::size_t seen = 0;
    for (const auto& part : parts) {
        if (part == "/") return std::nullopt;
        if (auto style = parse_border_style(part)) {
            for (std::size_t edge = 0; edge < 4; ++edge) result.styles[edge] = *style;
            for (std::size_t edge = 0; edge < 4; ++edge) result.style_present[edge] = true;
            ++seen;
            continue;
        }
        if (auto color = parse_color(part)) {
            for (std::size_t edge = 0; edge < 4; ++edge) {
                result.colors[edge] = *color;
                result.color_present[edge] = true;
            }
            ++seen;
            continue;
        }
        if (auto width = parse_length(part)) {
            if (width->is_auto) return std::nullopt;
            for (std::size_t edge = 0; edge < 4; ++edge) {
                result.widths[edge] = std::abs(width->value);
                result.width_present[edge] = true;
            }
            ++seen;
            continue;
        }
        return std::nullopt;  // `thin`/`medium`/`thick` and unknown tokens
    }
    if (seen == 0) return std::nullopt;
    return result;
}

std::optional<std::string> parse_url_token(std::string_view value) {
    const auto text = trim(value);
    if (iequals(text, "none")) return std::nullopt;
    std::string_view name, args;
    if (!open_function(text, name, args)) return std::nullopt;
    if (lowered(name) != "url") return std::nullopt;
    // Strip an optional quoted form; escapes are preserved verbatim because the
    // resolver, not this stratum, decides how to decode them.
    auto body = trim(args);
    if (body.size() >= 2 && (body.front() == '"' || body.front() == '\'') &&
        body.back() == body.front()) {
        body = body.substr(1, body.size() - 2);
    }
    if (body.empty()) return std::nullopt;
    return std::string(body);
}

std::vector<std::string> split_selector_list(std::string_view selectors) {
    std::vector<std::string> parts;
    std::string current;
    int depth = 0;
    char quote = 0;
    for (std::size_t i = 0; i < selectors.size(); ++i) {
        const char c = selectors[i];
        if (quote != 0) {
            current.push_back(c);
            if (c == '\\' && i + 1 < selectors.size()) current.push_back(selectors[++i]);
            else if (c == quote) quote = 0;
            continue;
        }
        if (c == '"' || c == '\'') { quote = c; current.push_back(c); continue; }
        if (c == '(' || c == '[') { ++depth; current.push_back(c); continue; }
        if (c == ')' || c == ']') { if (depth > 0) --depth; current.push_back(c); continue; }
        if (depth == 0 && c == ',') {
            const auto trimmed = trim(current);
            if (!trimmed.empty()) parts.emplace_back(trimmed);
            current.clear();
            continue;
        }
        current.push_back(c);
    }
    const auto trimmed = trim(current);
    if (!trimmed.empty()) parts.emplace_back(trimmed);
    return parts;
}

void compute_specificity(std::string_view selector, CssRule& rule) {
    // (id, class, type) counts over the selector, which is what the cascade
    // compares. Counted lexically rather than through the Flatworm selector
    // model so a selector the matching layer rejects is still ordered.
    unsigned ids = 0, classes = 0, types = 0;
    char quote = 0;
    bool in_attribute = false;
    for (std::size_t i = 0; i < selector.size(); ++i) {
        const char c = selector[i];
        if (quote != 0) {
            if (c == '\\') { ++i; continue; }
            if (c == quote) quote = 0;
            continue;
        }
        if (c == '"' || c == '\'') { quote = c; continue; }
        if (in_attribute) {
            if (c == ']') in_attribute = false;
            continue;
        }
        if (c == '[') { ++classes; in_attribute = true; continue; }
        if (c == '.') { ++classes; continue; }
        if (c == '#') { ++ids; continue; }
        if (c == ':') {
            // `::` is a pseudo-element; both count as a class for ordering.
            ++classes;
            if (i + 1 < selector.size() && selector[i + 1] == ':') ++i;
            continue;
        }
        if (c == '>' || c == '+' || c == '~' || c == ' ' || c == '\t' || c == '\n' ||
            c == '\r' || c == '\f') {
            continue;
        }
        if (c == '*') continue;
        if (c == '(' || c == ')' || c == ',') continue;
        // A type or universal position: count one type component.
        ++types;
        // Skip the identifier itself so its characters are not recounted.
        while (i + 1 < selector.size() &&
               (std::isalnum(static_cast<unsigned char>(selector[i + 1])) != 0 ||
                selector[i + 1] == '-' || selector[i + 1] == '_' || selector[i + 1] == '\\')) {
            ++i;
        }
    }
    rule.specificity_id = ids;
    rule.specificity_class = classes;
    rule.specificity_type = types;
}

// --- stylesheet parsing ------------------------------------------------------

namespace {

// Finds the matching close brace for the `{` at `open`, honouring strings and
// nesting. Returns npos when the block is unterminated.
std::size_t find_block_end(std::string_view css, std::size_t open) {
    int depth = 0;
    char quote = 0;
    for (std::size_t i = open; i < css.size(); ++i) {
        const char c = css[i];
        if (quote != 0) {
            if (c == '\\') { ++i; continue; }
            if (c == quote) quote = 0;
            continue;
        }
        if (c == '/' && i + 1 < css.size() && css[i + 1] == '*') {
            const auto close = css.find("*/", i + 2);
            if (close == std::string_view::npos) return std::string_view::npos;
            i = close + 1;
            continue;
        }
        if (c == '"' || c == '\'') { quote = c; continue; }
        if (c == '{') { ++depth; continue; }
        if (c == '}') {
            --depth;
            if (depth == 0) return i;
        }
    }
    return std::string_view::npos;
}

// True when a `@media` prelude should be applied unconditionally. Only the
// media types the layout subset can answer are honoured; `screen` and `all`
// match, and an unparsable prelude is treated as matching so content is not
// silently dropped from a page that uses media queries decoratively.
bool media_applies(std::string_view prelude) {
    const auto text = trim(prelude);
    if (text.empty()) return true;
    // Take the leading media type list, stopping at `and`/`only`.
    std::size_t end = 0;
    while (end < text.size() && text[end] != '(') ++end;
    const auto types = trim(text.substr(0, end));
    if (types.empty()) return true;
    bool saw_known = false;
    for (const auto& piece : split_top_level(types, true)) {
        if (piece == "," || piece == "/" || piece.empty()) continue;
        if (piece == "all" || piece == "screen") { saw_known = true; continue; }
        // `print`, `speech`, or a feature condition: not applied.
        return false;
    }
    return saw_known || types.size() == 0;
}

std::vector<Declaration> parse_declaration_block(std::string_view body,
                                                 std::size_t& declaration_budget) {
    std::vector<Declaration> declarations;
    std::size_t i = 0;
    while (i <= body.size()) {
        // One declaration runs to the next top-level `;`.
        std::size_t end = i;
        int depth = 0;
        char quote = 0;
        for (; end < body.size(); ++end) {
            const char c = body[end];
            if (quote != 0) {
                if (c == '\\') { ++end; continue; }
                if (c == quote) quote = 0;
                continue;
            }
            if (c == '"' || c == '\'') { quote = c; continue; }
            if (c == '(' || c == '[') { ++depth; continue; }
            if (c == ')' || c == ']') { if (depth > 0) --depth; continue; }
            if (c == ';' && depth == 0) break;
        }
        const auto text = trim(body.substr(i, end - i));
        i = end + 1;
        if (text.empty()) {
            if (end >= body.size()) break;
            continue;
        }
        const auto colon = text.find(':');
        if (colon == std::string_view::npos) {
            if (end >= body.size()) break;
            continue;
        }
        const auto property = trim(text.substr(0, colon));
        auto value = trim(text.substr(colon + 1));
        if (property.empty() || value.empty()) {
            if (end >= body.size()) break;
            continue;
        }
        bool important = false;
        // `!important` is a token; strip it case-insensitively before the last
        // `!` that is not inside a string (strings were already skipped).
        const auto bang = value.rfind('!');
        if (bang != std::string_view::npos) {
            const auto tail = trim(value.substr(bang + 1));
            if (iequals(tail, "important")) {
                important = true;
                value = trim(value.substr(0, bang));
            }
        }
        if (value.empty()) {
            if (end >= body.size()) break;
            continue;
        }
        if (declaration_budget == 0) {
            throw Error(ErrorCode::ResourceLimit, "stylesheet declaration budget");
        }
        --declaration_budget;
        // Custom properties are recorded but never interpreted: the layout
        // subset does not resolve var(), so storing them would imply support.
        if (!property.starts_with("--")) {
            declarations.push_back({std::string(property), std::string(value), important});
        }
        if (end >= body.size()) break;
    }
    return declarations;
}

// Parses rules in `css` into `out`, recursing into `@media` blocks that apply.
void parse_into(std::string_view css, std::vector<CssRule>& out,
                std::size_t& rule_budget, std::size_t& declaration_budget, unsigned nesting = 0) {
    if (nesting > 32) throw Error(ErrorCode::ResourceLimit, "stylesheet nesting budget");
    std::size_t i = 0;
    while (i < css.size()) {
        // Skip whitespace and comments between rules.
        if (is_space(css[i])) { ++i; continue; }
        if (css[i] == '/' && i + 1 < css.size() && css[i + 1] == '*') {
            const auto close = css.find("*/", i + 2);
            if (close == std::string_view::npos) return;
            i = close + 2;
            continue;
        }
        if (css[i] == '}') { ++i; continue; }
        if (css[i] == '@') {
            // At-rule: apply `@media` when its query matches, otherwise skip
            // the whole block. `@import`/`@charset`/`@font-face` and friends are
            // skipped because the subset loads no external resources.
            const auto brace = css.find('{', i);
            const auto semi = css.find(';', i);
            if (brace == std::string_view::npos) return;
            if (semi != std::string_view::npos && semi < brace) { i = semi + 1; continue; }
            const auto prelude = trim(css.substr(i + 1, brace - i - 1));
            const auto end = find_block_end(css, brace);
            if (end == std::string_view::npos) return;
            const auto keyword = prelude.substr(0, prelude.find_first_of(" \t\n"));
            if (iequals(keyword, "media") && media_applies(trim(prelude.substr(keyword.size())))) {
                parse_into(css.substr(brace + 1, end - brace - 1), out, rule_budget,
                           declaration_budget, nesting + 1);
            }
            i = end + 1;
            continue;
        }
        const auto brace = css.find('{', i);
        if (brace == std::string_view::npos) return;
        const auto prelude = trim(css.substr(i, brace - i));
        const auto end = find_block_end(css, brace);
        if (end == std::string_view::npos) return;
        if (!prelude.empty()) {
            auto declarations = parse_declaration_block(
                css.substr(brace + 1, end - brace - 1), declaration_budget);
            if (!declarations.empty()) {
                const auto selectors = split_selector_list(prelude);
                for (const auto& selector : selectors) {
                    if (rule_budget == 0) {
                        throw Error(ErrorCode::ResourceLimit, "stylesheet rule budget");
                    }
                    --rule_budget;
                    CssRule rule;
                    rule.selector = selector;
                    if (declarations.size() > declaration_budget)
                        throw Error(ErrorCode::ResourceLimit, "expanded stylesheet declaration budget");
                    declaration_budget -= declarations.size();
                    rule.declarations = declarations;
                    rule.order = out.size();
                    compute_specificity(rule.selector, rule);
                    out.push_back(std::move(rule));
                }
            }
        }
        i = end + 1;
    }
}

}  // namespace

} // namespace prowsetk::detail

namespace prowsetk {
std::vector<CssRule> parse_stylesheet(std::string_view css) {
    if (css.size() > max_render_stylesheet_bytes) {
        throw Error(ErrorCode::ResourceLimit, "stylesheet size limit");
    }
    std::vector<CssRule> rules;
    std::size_t rule_budget = max_render_rules;
    std::size_t declaration_budget = max_render_declarations;
    detail::parse_into(css, rules, rule_budget, declaration_budget);
    return rules;
}

}  // namespace prowsetk
