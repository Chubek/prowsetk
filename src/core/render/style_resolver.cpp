// style_resolver.cpp — style stratum: the cascade.
//
// Owns which declarations win and what the resulting computed values are. It
// merges three origins — the user-agent stylesheet, author rules from `<style>`
// elements, and inline `style` attributes — by importance, origin and
// specificity, then resolves inheritance and relative units into computed
// values. It computes no geometry and paints nothing; those belong to the layout
// and paint strata.
//
// Selector matching is Flatworm's, not this file's: `Element::matches` is the
// same engine that serves `querySelector`, so CSS cannot disagree with the page
// about what a selector means. To keep the cascade affordable, rules are
// bucketed by the rightmost compound's id/class/tag key, so an element only
// evaluates rules that could possibly match it.
//
// The user-agent stylesheet is written as CSS and parsed by the same parser, so
// there is exactly one cascade implementation to reason about.

#include "core/render/css_syntax.hpp"
#include "core/render/render_internal.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "flatworm/dom_internal.hpp"
#include <prowsetk/error.hpp>
#include <prowsetk/url.hpp>

namespace prowsetk::detail {
namespace {

// Origins, lowest priority first.
enum class Origin { UserAgent = 0, Author = 1 };

// A declaration that survived the cascade for one element, with the key it is
// ordered by. `beats` is the whole cascade: importance, then origin, then
// (id, class, type) specificity, then source order.
struct Declared {
    std::string value;
    bool important = false;
    int origin = 0;
    unsigned id = 0;
    unsigned cls = 0;
    unsigned type = 0;
    std::size_t order = 0;

    bool beats(const Declared& other) const {
        if (important != other.important) return important;
        if (origin != other.origin) return origin > other.origin;
        if (id != other.id) return id > other.id;
        if (cls != other.cls) return cls > other.cls;
        if (type != other.type) return type > other.type;
        return order >= other.order;
    }
};

using DeclaredMap = std::unordered_map<std::string, Declared>;

// --- rule bucketing ----------------------------------------------------------

// Key of a rule, taken from its selector's rightmost compound. An element can
// only match a rule whose key it shares, so a bucket miss is a guaranteed
// non-match. A rule with no id/class/tag in that compound gets the empty key,
// which every element shares.
struct BucketKey {
    std::string id;
    std::string cls;
    std::string tag;

    bool operator==(const BucketKey&) const noexcept = default;
};

struct BucketKeyHash {
    std::size_t operator()(const BucketKey& key) const noexcept {
        std::size_t seed = std::hash<std::string>{}(key.id);
        seed ^= std::hash<std::string>{}(key.cls) + 0x9e3779b9u + (seed << 6) + (seed >> 2);
        seed ^= std::hash<std::string>{}(key.tag) + 0x9e3779b9u + (seed << 6) + (seed >> 2);
        return seed;
    }
};

struct RuleRef {
    const CssRule* rule = nullptr;
    int origin = 0;
};

struct BucketedSheet {
    std::vector<RuleRef> universal;
    std::unordered_map<BucketKey, std::vector<RuleRef>, BucketKeyHash> keyed;
};

BucketKey bucket_of(std::string_view selector) {
    BucketKey key;
    if (selector.find_first_of(":\\[()") != std::string_view::npos) return key;
    const auto is_combinator = [](char c) {
        return is_css_space(c) || c == '>' || c == '+' || c == '~';
    };
    std::size_t end = selector.size();
    while (end > 0 && is_combinator(selector[end - 1])) --end;
    if (end == 0) return key;
    std::size_t start = end;
    while (start > 0 && !is_combinator(selector[start - 1])) --start;
    const auto compound = selector.substr(start, end - start);

    // Leading tag name, if the compound starts with one.
    std::size_t i = 0;
    while (i < compound.size() && compound[i] != '#' && compound[i] != '.' &&
           compound[i] != '[' && compound[i] != ':') {
        ++i;
    }
    if (i > 0) {
        std::string tag(compound.substr(0, i));
        std::transform(tag.begin(), tag.end(), tag.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (tag != "*") key.tag = std::move(tag);
    }

    // First id and class in the compound. Pseudo-classes and attribute
    // selectors are not keyed: they are common and only narrow further.
    bool quote = false;
    bool bracket = false;
    for (std::size_t j = 0; j < compound.size(); ++j) {
        const char c = compound[j];
        if (quote) {
            if (c == '\'' || c == '"') quote = false;
            continue;
        }
        if (c == '\'' || c == '"') { quote = true; continue; }
        if (c == '[') { bracket = true; continue; }
        if (c == ']') { bracket = false; continue; }
        if (bracket) continue;
        if ((c == '#' || c == '.') && key.id.empty() && key.cls.empty()) {
            std::size_t k = j + 1;
            while (k < compound.size() && compound[k] != '#' && compound[k] != '.' &&
                   compound[k] != '[' && compound[k] != ':' && compound[k] != ')' &&
                   !is_css_space(compound[k])) {
                ++k;
            }
            if (c == '#') key.id = std::string(compound.substr(j + 1, k - j - 1));
            else key.cls = std::string(compound.substr(j + 1, k - j - 1));
        }
    }
    return key;
}

// True when an element's own id/classes/tag can match `key`.
bool bucket_compatible(const BucketKey& key, const Element& element) {
    if (!key.tag.empty()) {
        std::string tag = element.tag_name();
        std::transform(tag.begin(), tag.end(), tag.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (tag != key.tag) return false;
    }
    if (!key.id.empty() && element.id() != key.id) return false;
    if (!key.cls.empty() && element.class_name().find(key.cls) == std::string::npos) {
        return false;
    }
    return true;
}

BucketedSheet bucket(const std::vector<CssRule>& rules,
                     const std::vector<int>& origins) {
    BucketedSheet sheet;
    for (std::size_t i = 0; i < rules.size(); ++i) {
        const RuleRef ref{&rules[i], origins[i]};
        const auto key = bucket_of(rules[i].selector);
        if (key.id.empty() && key.cls.empty() && key.tag.empty()) {
            sheet.universal.push_back(ref);
            continue;
        }
        sheet.keyed[key].push_back(ref);
    }
    return sheet;
}

void apply(const RuleRef& ref, DeclaredMap& target, std::size_t& order_counter) {
    order_counter = 0;
    for (const auto& declaration : ref.rule->declarations) {
        Declared candidate;
        candidate.value = declaration.value;
        candidate.important = declaration.important;
        candidate.origin = ref.origin;
        candidate.id = ref.rule->specificity_id;
        candidate.cls = ref.rule->specificity_class;
        candidate.type = ref.rule->specificity_type;
        candidate.order = ref.rule->order * (max_render_declarations + 1) + order_counter++ % (max_render_declarations + 1);
        const auto found = target.find(declaration.property);
        if (found == target.end() || candidate.beats(found->second)) {
            target[declaration.property] = std::move(candidate);
        }
    }
}

// --- inline `style` attribute ------------------------------------------------

// Trims without allocating: the attribute is page-controlled and can be large.
std::string_view trim_view(std::string_view text) {
    while (!text.empty() && (std::isspace(static_cast<unsigned char>(text.front())) != 0)) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (std::isspace(static_cast<unsigned char>(text.back())) != 0)) {
        text.remove_suffix(1);
    }
    return text;
}

void apply_inline(std::string_view style_text, DeclaredMap& target,
                  std::size_t& order_counter) {
    // An inline declaration has no selector, so it outranks every author rule of
    // equal importance. Encoding that as a large specificity keeps the whole
    // cascade one integer comparison.
    const int origin = static_cast<int>(Origin::Author);
    constexpr unsigned inline_specificity = 1u << 20;
    std::size_t i = 0;
    while (i <= style_text.size()) {
        std::size_t end = i;
        int depth = 0;
        char quote = 0;
        for (; end < style_text.size(); ++end) {
            const char c = style_text[end];
            if (quote != 0) {
                if (c == '\\') { ++end; continue; }
                if (c == quote) quote = 0;
                continue;
            }
            if (c == '\'' || c == '"') { quote = c; continue; }
            if (c == '(' || c == '[') { ++depth; continue; }
            if (c == ')' || c == ']') { if (depth > 0) --depth; continue; }
            if (c == ';' && depth == 0) break;
        }
        const auto text = style_text.substr(i, end - i);
        i = end + 1;
        const auto colon = text.find(':');
        if (colon != std::string_view::npos) {
            const auto property = trim_view(text.substr(0, colon));
            auto value = trim_view(text.substr(colon + 1));
            bool important = false;
            const auto bang = value.rfind('!');
            if (bang != std::string_view::npos &&
                iequals(trim_view(value.substr(bang + 1)), "important")) {
                important = true;
                value = trim_view(value.substr(0, bang));
            }
            // Custom properties are dropped: the subset does not resolve var(),
            // so recording them would imply support it does not have.
            if (!property.empty() && !value.empty() && !property.starts_with("--")) {
                Declared candidate;
                candidate.value = std::string(value);
                candidate.important = important;
                candidate.origin = origin;
                candidate.id = inline_specificity;
                candidate.order = order_counter++;
                const auto found = target.find(std::string(property));
                if (found == target.end() || candidate.beats(found->second)) {
                    target[std::string(property)] = std::move(candidate);
                }
            }
        }
        if (end >= style_text.size()) break;
    }
}

// --- shorthand expansion -----------------------------------------------------
// Expanded into longhands before computed values are read, so a later
// `margin-left` always beats an earlier `margin` regardless of source order.

std::string length_to_text(const Length& length) {
    if (length.is_auto) return "auto";
    std::string text = std::to_string(length.value);
    switch (length.unit) {
        case LengthUnit::Px: text += "px"; break;
        case LengthUnit::Em: text += "em"; break;
        case LengthUnit::Rem: text += "rem"; break;
        case LengthUnit::Percent: text += "%"; break;
        case LengthUnit::Vw: text += "vw"; break;
        case LengthUnit::Vh: text += "vh"; break;
        case LengthUnit::Vmin: text += "vmin"; break;
        case LengthUnit::Vmax: text += "vmax"; break;
        case LengthUnit::Auto: break;
    }
    return text;
}

void expand_box_shorthand(const DeclaredMap& in, DeclaredMap& out, const char* prefix) {
    const auto found = in.find(prefix);
    if (found == in.end()) return;
    const auto box = parse_box(found->second.value);
    // An unparsable shorthand is ignored rather than guessed at.
    if (!box) return;
    static constexpr std::array<std::string_view, 4> kEdges{"top", "right", "bottom",
                                                           "left"};
    for (std::size_t edge = 0; edge < 4; ++edge) {
        if (!box->present[edge]) continue;
        Declared part = found->second;
        const auto length = box->at(edge);
        part.value = length ? length_to_text(*length) : "auto";
        const std::string name =
            std::string(prefix) + "-" + std::string(kEdges[edge]);
        const auto existing = out.find(name);
        if (existing == out.end() || part.beats(existing->second)) {
            out[name] = std::move(part);
        }
    }
}

void expand_border_shorthands(const DeclaredMap& in, DeclaredMap& out) {
    static constexpr std::array<std::string_view, 4> kEdges{"top", "right", "bottom",
                                                           "left"};
    static constexpr std::array<const char*, 4> kShorthands{
        "border", "border-width", "border-color", "border-style"};
    for (const char* shorthand : kShorthands) {
        const auto found = in.find(shorthand);
        if (found == in.end()) continue;
        const auto parsed = parse_border_shorthand(found->second.value);
        if (!parsed) continue;
        const auto record = [&](const std::string& name, const std::string& value) {
            Declared part = found->second;
            part.value = value;
            const auto existing = out.find(name);
            if (existing == out.end() || part.beats(existing->second)) {
                out[name] = std::move(part);
            }
        };
        for (std::size_t edge = 0; edge < 4; ++edge) {
            const std::string suffix(kEdges[edge]);
            if (parsed->width_present[edge]) {
                record("border-" + suffix + "-width",
                       std::to_string(parsed->widths[edge]) + "px");
            }
            if (parsed->style_present[edge]) {
                const char* style = "none";
                switch (parsed->styles[edge]) {
                    case BorderStyle::Solid: style = "solid"; break;
                    case BorderStyle::Dashed: style = "dashed"; break;
                    case BorderStyle::Dotted: style = "dotted"; break;
                    case BorderStyle::Double: style = "double"; break;
                    case BorderStyle::Groove: style = "groove"; break;
                    case BorderStyle::Ridge: style = "ridge"; break;
                    case BorderStyle::None: break;
                }
                record("border-" + suffix + "-style", style);
            }
            if (parsed->color_present[edge]) {
                const auto& c = parsed->colors[edge];
                char buffer[16];
                std::snprintf(buffer, sizeof(buffer), "#%02x%02x%02x", c.r, c.g, c.b);
                record("border-" + suffix + "-color", buffer);
            }
        }
    }
}

void expand_font(const DeclaredMap& in, DeclaredMap& out) {
    const auto found = in.find("font");
    if (found == in.end()) return;
    const auto record = [&](const char* name, const std::string& value) {
        Declared part = found->second;
        part.value = value;
        const auto existing = out.find(name);
        if (existing == out.end() || part.beats(existing->second)) {
            out[std::string(name)] = std::move(part);
        }
    };
    // Only the common `[style] [weight] size[/line-height] family` shape is
    // expanded. Anything else falls through to the individual longhands, which
    // always win when both are present.
    const auto tokens = split_top_level(found->second.value, true);
    std::size_t i = 0;
    for (; i < tokens.size(); ++i) {
        if (tokens[i] == "," || tokens[i] == "/") continue;
        if (iequals(tokens[i], "italic") || iequals(tokens[i], "oblique")) {
            record("font-style", "italic");
            continue;
        }
        if (iequals(tokens[i], "normal")) { record("font-style", "normal"); continue; }
        if (parse_font_weight(tokens[i])) { record("font-weight", tokens[i]); continue; }
        break;
    }
    if (i < tokens.size() && tokens[i] != "," && tokens[i] != "/") {
        auto size_text = tokens[i];
        std::string line_text;
        const auto slash = size_text.find('/');
        if (slash != std::string::npos) {
            line_text = size_text.substr(slash + 1);
            size_text = size_text.substr(0, slash);
        }
        record("font-size", size_text);
        if (!line_text.empty()) record("line-height", line_text);
        ++i;
    }
    std::string family;
    for (; i < tokens.size(); ++i) {
        if (tokens[i] == ",") { family.clear(); continue; }
        if (!family.empty()) family += " ";
        family += tokens[i];
    }
    if (!family.empty()) record("font-family", family);
}

void expand_list_style(const DeclaredMap& in, DeclaredMap& out) {
    const auto found = in.find("list-style");
    if (found == in.end()) return;
    const auto record = [&](const char* name, const std::string& value) {
        Declared part = found->second;
        part.value = value;
        const auto existing = out.find(name);
        if (existing == out.end() || part.beats(existing->second)) {
            out[std::string(name)] = std::move(part);
        }
    };
    for (const auto& token : split_top_level(found->second.value, true)) {
        if (parse_list_style_type(token)) record("list-style-type", token);
        else if (parse_list_style_position(token)) record("list-style-position", token);
    }
}

void expand_shorthands(const DeclaredMap& in, DeclaredMap& out) {
    out = in;
    expand_box_shorthand(in, out, "margin");
    expand_box_shorthand(in, out, "padding");
    expand_border_shorthands(in, out);
    expand_font(in, out);
    expand_list_style(in, out);
    const auto record = [&](const Declared& source, const char* name, std::string value) {
        Declared part = source;
        part.value = std::move(value);
        const auto existing = out.find(name);
        if (existing == out.end() || part.beats(existing->second)) out[name] = std::move(part);
    };
    if (const auto it = in.find("flex"); it != in.end()) {
        if (const auto parsed = parse_flex(it->second.value)) {
            record(it->second, "flex-grow", std::to_string(parsed->grow));
            record(it->second, "flex-shrink", std::to_string(parsed->shrink));
            record(it->second, "flex-basis", length_to_text(parsed->basis));
        }
    }
    if (const auto it = in.find("gap"); it != in.end()) {
        const auto tokens = split_top_level(it->second.value, true);
        if (!tokens.empty() && tokens.size() <= 2) {
            const auto row = parse_length(tokens[0]);
            const auto column = parse_length(tokens.back());
            if (row && column && !row->is_auto && !column->is_auto && row->value >= 0 && column->value >= 0) {
                record(it->second, "row-gap", tokens[0]);
                record(it->second, "column-gap", tokens.back());
            }
        }
    }
    // Solid-color backgrounds are the supported shorthand slice. Do not turn
    // gradients or URL layers into a guessed color.
    if (const auto it = in.find("background"); it != in.end()) {
        if (parse_color(it->second.value)) record(it->second, "background-color", it->second.value);
    }
}

// --- inherited properties ----------------------------------------------------

bool inherits(std::string_view property) {
    static constexpr std::array<std::string_view, 18> kInherited{
        "color", "font", "font-family", "font-size", "font-style", "font-weight",
        "line-height", "list-style", "list-style-type", "list-style-position",
        "text-align", "text-transform", "visibility", "white-space",
        "word-spacing", "letter-spacing", "cursor", "direction",
    };
    return std::find(kInherited.begin(), kInherited.end(), property) != kInherited.end();
}

// `text-decoration` is not inherited in CSS but propagates to descendants of an
// inline; carrying it here keeps an anchor's underline on its own text runs.
bool propagates_to_descendants(std::string_view property) {
    return property == "text-decoration" || property == "text-decoration-line";
}

// --- unit resolution ---------------------------------------------------------

double resolve_px(const Length& length, double font_size, double root_font_size,
                  double viewport_width, double viewport_height,
                  double percent_basis) {
    if (length.is_auto) return 0.0;
    switch (length.unit) {
        case LengthUnit::Px: return length.value;
        case LengthUnit::Em: return length.value * font_size;
        case LengthUnit::Rem: return length.value * root_font_size;
        case LengthUnit::Percent: return percent_basis * length.value / 100.0;
        case LengthUnit::Vw: return viewport_width * length.value / 100.0;
        case LengthUnit::Vh: return viewport_height * length.value / 100.0;
        case LengthUnit::Vmin:
            return std::min(viewport_width, viewport_height) * length.value / 100.0;
        case LengthUnit::Vmax:
            return std::max(viewport_width, viewport_height) * length.value / 100.0;
        case LengthUnit::Auto: return 0.0;
    }
    return 0.0;
}

Insets to_insets(const BoxValues& box, double font_size, double root_font_size,
                 double viewport_width, double viewport_height,
                 double percent_basis, bool clamp_negative) {
    Insets insets;
    const double values[4] = {
        resolve_px(box.top.value_or(Length{}), font_size, root_font_size,
                   viewport_width, viewport_height, percent_basis),
        resolve_px(box.right.value_or(Length{}), font_size, root_font_size,
                   viewport_width, viewport_height, percent_basis),
        resolve_px(box.bottom.value_or(Length{}), font_size, root_font_size,
                   viewport_width, viewport_height, percent_basis),
        resolve_px(box.left.value_or(Length{}), font_size, root_font_size,
                   viewport_width, viewport_height, percent_basis),
    };
    insets.top = values[0];
    insets.right = values[1];
    insets.bottom = values[2];
    insets.left = values[3];
    // Padding and border never take a negative used length; margins may.
    if (clamp_negative) {
        for (double* value : {&insets.top, &insets.right, &insets.bottom, &insets.left}) {
            if (*value < 0.0) *value = 0.0;
        }
    }
    return insets;
}

ComputedStyle initial_style(const StyleContext& context) {
    ComputedStyle style;
    style.display = Display::Inline;
    style.color = Color{0, 0, 0, 255};
    style.background_color = Color{0, 0, 0, 0};
    style.font.family = "sans-serif";
    style.font.size = context.root_font_size;
    style.font.weight = 400;
    style.position = Position::Static;
    return style;
}

// --- computed value resolution ----------------------------------------------

ComputedStyle compute(const Element& element, const DeclaredMap& declared,
                      const ComputedStyle& parent, const StyleContext& context,
                      bool is_root) {
    ComputedStyle style = parent;
    // Non-inherited properties start from their initial values, not the parent's.
    style.display = Display::Inline;
    style.position = Position::Static;
    style.background_color = Color{0, 0, 0, 0};
    style.background_image.clear();
    style.opacity = 1.0;
    style.overflow = Overflow::Visible;
    style.underline = false;
    style.line_through = false;
    style.object_fit = ObjectFit::Fill;
    style.replaced_source.clear();
    style.replaced_alt.clear();
    style.is_replaced = false;
    style.control = ControlKindHint::None;
    style.control_type.clear();
    style.control_value.clear();
    style.control_placeholder.clear();
    style.control_checked = false;
    style.control_disabled = false;
    style.control_read_only = false;
    style.control_options.clear();
    style.offset_x = Length::auto_length();
    style.offset_y = Length::auto_length();
    style.cursor.clear();
    style.width = Length::auto_length();
    style.height = Length::auto_length();
    style.min_width = Length::auto_length();
    style.min_height = Length::auto_length();
    style.max_width = Length::auto_length();
    style.max_height = Length::auto_length();
    style.box_sizing = BoxSizing::ContentBox;
    style.margin_left_auto = style.margin_right_auto = false;
    style.flex_direction = FlexDirection::Row;
    style.flex_wrap = FlexWrap::NoWrap;
    style.justify_content = JustifyContent::Start;
    style.align_items = AlignItems::Stretch;
    style.align_self.reset();
    style.flex_grow = 0;
    style.flex_shrink = 1;
    style.flex_basis = Length::auto_length();
    style.row_gap = style.column_gap = Length::px(0);
    style.order = 0;
    style.margin = Insets{};
    style.padding = Insets{};
    for (double& width : style.border_width) width = 0.0;
    style.border_style = BorderStyle::None;
    for (auto& color : style.border_color) color = Color{0, 0, 0, 255};
    if (is_root) style.display = Display::Block;

    std::string tag = element.tag_name();
    std::transform(tag.begin(), tag.end(), tag.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    const auto get = [&declared](const char* name) -> const Declared* {
        const auto found = declared.find(name);
        return found == declared.end() ? nullptr : &found->second;
    };

    // Font size first: em, rem and percentage sizes below resolve against it.
    double font_size = parent.font.size;
    if (const auto* value = get("font-size")) {
        if (const auto size = parse_font_size(value->value, parent.font.size)) {
            // Bounded so a hostile sheet cannot make text unmeasurable.
            if (*size > 0.0 && *size <= 512.0) font_size = *size;
        }
    }
    style.font.size = font_size;
    if (const auto* value = get("font-family")) {
        if (const auto family = parse_font_family(value->value)) {
            if (family->size() > 1024) throw Error(ErrorCode::ResourceLimit, "render font budget");
            if (!family->empty()) style.font.family = *family;
        }
    }
    if (const auto* value = get("font-weight")) {
        if (const auto weight = parse_font_weight(value->value)) {
            style.font.weight = static_cast<int>(*weight);
        }
    }
    if (const auto* value = get("font-style")) {
        const auto text = lowered(value->value);
        style.font.italic = text == "italic" || text == "oblique";
    }
    if (const auto* value = get("display")) {
        if (const auto display = parse_display(value->value)) {
            style.display = *display;
        }
    }
    if (const auto* value = get("position")) {
        if (const auto parsed = parse_position(value->value)) style.position = *parsed;
    }
    if (const auto* value = get("box-sizing")) {
        if (iequals(value->value, "border-box")) style.box_sizing = BoxSizing::BorderBox;
    }
    if (const auto* value = get("flex-direction")) {
        if (iequals(value->value, "row-reverse")) style.flex_direction = FlexDirection::RowReverse;
        // Unsupported column layouts use normal block flow, rather than
        // silently laying a requested column out horizontally.
        if ((iequals(value->value, "column") || iequals(value->value, "column-reverse")) &&
            style.display == Display::Flex) style.display = Display::Block;
    }
    if (const auto* value = get("flex-wrap")) {
        if (iequals(value->value, "wrap")) style.flex_wrap = FlexWrap::Wrap;
    }
    if (const auto* value = get("justify-content")) {
        if (const auto parsed = parse_justify_content(value->value)) style.justify_content = *parsed;
    }
    if (const auto* value = get("align-items")) {
        if (const auto parsed = parse_align_items(value->value)) style.align_items = *parsed;
    }
    if (const auto* value = get("align-self")) style.align_self = parse_align_items(value->value);
    for (const auto& [name, target] : {std::pair{"flex-grow", &style.flex_grow},
                                     std::pair{"flex-shrink", &style.flex_shrink}}) {
        if (const auto* value = get(name)) {
            if (const auto parsed = parse_number(value->value); parsed && *parsed >= 0 && *parsed <= 1.0e6)
                *target = *parsed;
        }
    }
    if (const auto* value = get("order")) {
        if (const auto parsed = parse_number(value->value); parsed && std::abs(*parsed) <= 1000000 && std::floor(*parsed) == *parsed)
            style.order = static_cast<int>(*parsed);
    }
    for (const char* property : {"left", "top"}) {
        const auto* value = get(property);
        if (value == nullptr) continue;
        const auto length = parse_length(value->value);
        if (!length) continue;
        if (property[0] == 'l') style.offset_x = *length; else style.offset_y = *length;
    }
    if (const auto* value = get("color")) {
        if (const auto parsed = parse_color(value->value)) style.color = *parsed;
    }
    if (const auto* value = get("background-color")) {
        if (const auto parsed = parse_color(value->value)) style.background_color = *parsed;
    }
    if (const auto* value = get("background-image")) {
        if (const auto url = parse_url_token(value->value)) style.background_image = *url;
    }
    if (const auto* value = get("opacity")) {
        if (const auto parsed = parse_opacity(value->value)) style.opacity = *parsed;
    }
    if (const auto* value = get("line-height")) {
        // A number is a multiple of the font size; a length is used as written.
        if (const auto number = parse_number(value->value)) {
            style.line_height = *number * font_size;
        } else if (const auto length = parse_length(value->value)) {
            style.line_height = resolve_px(*length, font_size, context.root_font_size,
                                            context.viewport_width,
                                            context.viewport_height, font_size);
        }
    }
    if (const auto* value = get("text-align")) {
        if (const auto parsed = parse_text_align(value->value)) style.text_align = *parsed;
    }
    if (const auto* value = get("white-space")) {
        if (const auto parsed = parse_white_space(value->value)) style.white_space = *parsed;
    }
    if (const auto* value = get("overflow")) {
        if (const auto parsed = parse_overflow(value->value)) style.overflow = *parsed;
    }
    if (const auto* value = get("visibility")) {
        if (const auto visibility = parse_visibility(value->value)) {
            style.visibility = *visibility;
        }
    }
    if (const auto* value = get("vertical-align")) {
        if (const auto parsed = parse_vertical_align(value->value)) style.vertical_align = *parsed;
    }
    if (const auto* value = get("list-style-type")) {
        if (const auto type = parse_list_style_type(value->value)) {
            style.list_style_type = *type;
        }
    }
    if (const auto* value = get("list-style-position")) {
        if (const auto position = parse_list_style_position(value->value)) {
            style.list_style_position = *position;
        }
    }
    if (const auto* value = get("object-fit")) {
        if (const auto parsed = parse_object_fit(value->value)) style.object_fit = *parsed;
    }
    if (const auto* value = get("cursor")) {
        static constexpr std::array<std::string_view, 3> kCursors{"pointer", "default",
                                                                 "text"};
        const auto text = lowered(value->value);
        if (std::find(kCursors.begin(), kCursors.end(), std::string_view(text)) !=
            kCursors.end()) {
            style.cursor = text;
        }
    }
    for (const char* property : {"text-decoration", "text-decoration-line"}) {
        const auto* value = get(property);
        if (value == nullptr) continue;
        const auto text = lowered(value->value);
        style.underline = text.find("underline") != std::string::npos;
        style.line_through = text.find("line-through") != std::string::npos;
    }

    // Sizing keeps its units; layout resolves them against the containing block,
    // which only layout knows.
    struct SizeSlot { const char* property; Length* target; };
    const SizeSlot sizes[] = {
        {"width", &style.width},         {"height", &style.height},
        {"min-width", &style.min_width}, {"min-height", &style.min_height},
        {"max-width", &style.max_width}, {"max-height", &style.max_height},
        {"flex-basis", &style.flex_basis}, {"row-gap", &style.row_gap},
        {"column-gap", &style.column_gap},
    };
    for (const auto& slot : sizes) {
        const auto* value = get(slot.property);
        if (value == nullptr) continue;
        if (const auto length = parse_length(value->value); length && length->value >= 0) *slot.target = *length;
    }

    // Box model. Percentages here resolve against the viewport width, which is
    // the approximation the subset makes; the header says so.
    static constexpr std::array<std::string_view, 4> kEdges{"top", "right", "bottom",
                                                           "left"};
    BoxValues margins, padding;
    for (std::size_t edge = 0; edge < 4; ++edge) {
        const std::string margin_name = "margin-" + std::string(kEdges[edge]);
        if (const auto* value = get(margin_name.c_str())) {
            if (const auto box = parse_box(value->value)) {
                if (box->present[edge]) margins.set(edge, box->at(edge));
            }
        }
        const std::string padding_name = "padding-" + std::string(kEdges[edge]);
        if (const auto* value = get(padding_name.c_str())) {
            if (const auto box = parse_box(value->value)) {
                if (box->present[edge]) padding.set(edge, box->at(edge));
            }
        }
    }
    style.margin = to_insets(margins, font_size, context.root_font_size,
                             context.viewport_width, context.viewport_height,
                              context.viewport_width, false);
    style.margin_left_auto = margins.left && margins.left->is_auto;
    style.margin_right_auto = margins.right && margins.right->is_auto;
    style.padding = to_insets(padding, font_size, context.root_font_size,
                              context.viewport_width, context.viewport_height,
                              context.viewport_width, true);

    // Borders: one style and one colour for the element, applied to every edge,
    // with per-edge widths. That is the common authored case; a page using
    // per-edge colours still gets a visible border on each side.
    BorderStyle border_style = BorderStyle::None;
    for (const char* property : {"border-style", "border-top-style",
                                 "border-right-style", "border-bottom-style",
                                 "border-left-style"}) {
        const auto* value = get(property);
        if (value == nullptr) continue;
        if (const auto parsed = parse_border_style(value->value)) {
            if (border_style == BorderStyle::None) border_style = *parsed;
        }
    }
    Color border_color{0, 0, 0, 255};
    bool have_border_color = false;
    for (const char* property : {"border-color", "border-top-color",
                                 "border-right-color", "border-bottom-color",
                                 "border-left-color"}) {
        const auto* value = get(property);
        if (value == nullptr) continue;
        if (const auto parsed = parse_color(value->value)) {
            if (!have_border_color) { border_color = *parsed; have_border_color = true; }
        }
    }
    if (!have_border_color) border_color = parent.color;
    for (std::size_t edge = 0; edge < 4; ++edge) {
        const std::string suffix(kEdges[edge]);
        double width = 0.0;
        if (const auto* value = get(("border-" + suffix + "-width").c_str())) {
            if (const auto parsed = parse_length(value->value)) {
                if (!parsed->is_auto) width = std::abs(parsed->value);
            }
        }
        if (border_style == BorderStyle::None) width = 0.0;
        style.border_width[edge] = std::min(width, 64.0);
        style.border_style = border_style;
        style.border_color[edge] = border_color;
    }

    // Replaced elements and form controls.
    if (tag == "img") {
        style.is_replaced = true;
        style.replaced_source = element.attribute("src");
        style.replaced_alt = element.attribute("alt");
    } else if (tag == "canvas") {
        style.is_replaced = true;
        style.replaced_alt = element.attribute("aria-label");
    } else if (tag == "input") {
        auto type = lowered(element.attribute("type"));
        if (type.empty()) type = "text";
        style.control_type = type;
        style.control_disabled = element.has_attribute("disabled");
        style.control_read_only = element.has_attribute("readonly");
        style.control_checked = element.has_attribute("checked");
        style.control_value = element.value();
        style.control_placeholder = element.attribute("placeholder");
        if (type == "hidden") {
            style.display = Display::None;
            style.control = ControlKindHint::None;
        } else if (type == "checkbox") {
            style.control = ControlKindHint::Checkbox;
            // A checkbox paints its own indicator, so it is a replaced box.
            style.is_replaced = true;
        } else if (type == "radio") {
            style.control = ControlKindHint::Radio;
            style.is_replaced = true;
        } else if (type == "submit") {
            style.control = ControlKindHint::Submit;
        } else if (type == "button" || type == "reset" || type == "image") {
            style.control = ControlKindHint::Button;
        } else if (type == "password") {
            style.control = ControlKindHint::PasswordField;
        } else {
            style.control = ControlKindHint::TextField;
        }
    } else if (tag == "textarea") {
        style.control = ControlKindHint::TextArea;
        style.control_value = element.text();
        style.control_disabled = element.has_attribute("disabled");
        style.control_read_only = element.has_attribute("readonly");
        style.control_placeholder = element.attribute("placeholder");
    } else if (tag == "select") {
        style.control = ControlKindHint::Select;
        style.control_disabled = element.has_attribute("disabled");
        style.control_read_only = element.has_attribute("readonly");
        for (const auto& option : element.children()) {
            std::string option_tag = option->tag_name();
            std::transform(option_tag.begin(), option_tag.end(), option_tag.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (option_tag != "option") continue;
            const auto label = option->attribute("label");
            auto text = label.empty() ? option->text() : label;
            if (style.control_options.size() < 512) {
                style.control_options.push_back(std::move(text));
            }
        }
    } else if (tag == "button") {
        const auto type = lowered(element.attribute("type"));
        style.control = (type == "submit" || type == "reset")
                            ? ControlKindHint::Submit
                            : ControlKindHint::Button;
        style.control_disabled = element.has_attribute("disabled");
    }
    if (style.control != ControlKindHint::None) {
        style.control_value = "[redacted]";
        style.control_placeholder.clear();
        style.control_options.clear();
    }
    for (double value : {style.margin.top, style.margin.right, style.margin.bottom, style.margin.left,
                         style.padding.top, style.padding.right, style.padding.bottom, style.padding.left,
                         style.line_height}) {
        if (!std::isfinite(value) || std::abs(value) > max_render_content_pixels)
            throw Error(ErrorCode::ResourceLimit, "render geometry budget");
    }
    return style;
}

}  // namespace

std::string_view user_agent_stylesheet() {
    // The user-agent stylesheet of the subset: the standard HTML rendering hints
    // plus sizing for form controls, so a rendered page is readable and usable
    // rather than collapsed. Written as CSS and parsed by the same parser as
    // author CSS, so there is exactly one cascade.
    return R"CSS(
html, body, div, p, h1, h2, h3, h4, h5, h6, ul, ol, li, dl, dt, dd, blockquote,
pre, form, fieldset, legend, header, footer, main, nav, section, article, aside,
figure, figcaption, address, hr, table, thead, tbody, tfoot, tr, details,
summary, hgroup, output, fieldset, legend, center, menu, dir, optgroup {
  display: block;
}
head, script, style, template, meta, link, title, base, param, source, track,
datalist { display: none; }
li { display: list-item; }
table { display: table; }
thead, tbody, tfoot, tr { display: table-row; }
td, th { display: table-cell; }
caption { display: table-caption; }
body { margin: 8px; }
p { margin: 1em 0; }
h1 { font-size: 2em; font-weight: bold; margin: 0.67em 0; }
h2 { font-size: 1.5em; font-weight: bold; margin: 0.83em 0; }
h3 { font-size: 1.17em; font-weight: bold; margin: 1em 0; }
h4 { font-weight: bold; margin: 1.33em 0; }
h5 { font-size: 0.83em; font-weight: bold; margin: 1.67em 0; }
h6 { font-size: 0.67em; font-weight: bold; margin: 2.33em 0; }
blockquote { margin: 1em 40px; }
figure { margin: 1em 40px; }
ul, ol, menu, dir { margin: 1em 0; padding-left: 40px; list-style-type: disc; }
ul ul, ul ol, ol ul, ol ol, menu ul, dir ul { margin: 0; }
dl { margin: 1em 0; }
dd { margin-left: 40px; }
hr { margin: 0.5em auto; border-width: 1px; border-style: inset; }
pre { font-family: monospace; white-space: pre; margin: 1em 0; }
code, kbd, samp, tt { font-family: monospace; }
b, strong, th { font-weight: bold; }
i, em, cite, var, dfn, address { font-style: italic; }
u, ins { text-decoration: underline; }
s, strike, del { text-decoration: line-through; }
a[href] { cursor: pointer; }
small { font-size: 0.83em; }
big { font-size: 1.17em; }
sub { vertical-align: sub; font-size: 0.83em; }
sup { vertical-align: super; font-size: 0.83em; }
mark { background-color: #ffff00; color: #000000; }
center { text-align: center; }
table { border-spacing: 2px; }
caption { text-align: center; }
th { text-align: center; }
td, th, tr { vertical-align: middle; }
ol, ol ul, ol ol { list-style-type: decimal; }
ul ul, ul ol { list-style-type: circle; }
ul ul ul, ul ol ul, ul ul ol, ul ol ol { list-style-type: square; }
form { margin-top: 0; }
fieldset { margin: 0 2px; padding: 0.35em 0.75em 0.625em; border-width: 2px;
           border-style: groove; }
legend { padding: 0 2px; }
input, textarea, select, button { display: inline-block; font-size: 1em; }
input { width: 20em; height: 2em; padding: 1px 2px; border-width: 1px;
        border-style: inset; }
textarea { width: 30em; height: 4em; padding: 2px; border-width: 1px;
           border-style: inset; white-space: pre-wrap; }
button, input[type=button], input[type=submit], input[type=reset] {
  width: auto; height: auto; padding: 1px 6px; border-width: 1px;
  border-style: outset; text-align: center; cursor: pointer;
}
select { width: auto; height: 2em; padding: 1px; border-width: 1px;
         border-style: solid; }
input[type=checkbox], input[type=radio] { width: 1.2em; height: 1.2em;
                                          padding: 0; }
input[type=hidden] { display: none; }
summary { cursor: pointer; font-weight: bold; }
[hidden] { display: none; }
dialog:not([open]) { display: none; }
)CSS";
}

namespace {

// Collects `<style>` element text in document order. External stylesheets are
// not fetched by the core; a host that retrieved one supplies its text through
// the same path, so there is one cascade either way.
std::string collect_style_text(const flatworm::Node& root, bool& limited) {
    std::string combined;
    std::vector<std::shared_ptr<flatworm::Node>> pending;
    for (auto it = root.children.rbegin(); it != root.children.rend(); ++it) {
        pending.push_back(*it);
    }
    std::size_t depth = 0;
    while (!pending.empty()) {
        auto node = std::move(pending.back());
        pending.pop_back();
        if (++depth > max_render_elements) { limited = true; break; }
        if (node == nullptr) continue;
        if (node->type == flatworm::NodeType::Element) {
            std::string tag = node->name;
            std::transform(tag.begin(), tag.end(), tag.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (tag == "style") {
                const auto content = flatworm::text_content(*node);
                if (combined.size() + content.size() > max_render_stylesheet_bytes) {
                    limited = true;
                    break;
                }
                combined.append(content);
                combined.push_back('\n');
            }
        }
        for (auto it = node->children.rbegin(); it != node->children.rend(); ++it) {
            pending.push_back(*it);
        }
    }
    return combined;
}

}  // namespace

}  // namespace detail

namespace prowsetk {

// The public cascade entry point. Everything it uses lives in
// `prowsetk::detail`; a `using` keeps the body readable without qualifying
// every internal name.
using namespace prowsetk::detail;

ComputedStyles resolve_computed_styles(const Document& document,
                                       double viewport_width,
                                       double viewport_height,
                                       double root_font_size) {
    const StyleContext context{viewport_width, viewport_height,
                                       root_font_size};
    if (!std::isfinite(viewport_width) || !std::isfinite(viewport_height) ||
        !std::isfinite(root_font_size) || viewport_width <= 0 || viewport_height <= 0 ||
        viewport_width > max_render_viewport_dimension || viewport_height > max_render_viewport_dimension ||
        root_font_size <= 0 || root_font_size > 512)
        throw Error(ErrorCode::InvalidArgument, "invalid render dimensions");
    auto data = std::make_unique<ComputedStyles::Data>();
    const auto& raw_root = document.raw_root();
    if (raw_root == nullptr) {
        data->root = initial_style(context);
        return ComputedStyles(std::move(data));
    }

    // Elements in document order with their parent index, matching
    // `Document::query_selector_all("*")` so an index into one is an index into
    // the other. The raw root is a document node, so its children are the
    // top-level elements.
    std::vector<std::shared_ptr<Element>> elements;
    std::vector<std::size_t> parent_index;
    {
        std::vector<std::pair<std::shared_ptr<flatworm::Node>, std::size_t>> pending;
        for (auto it = raw_root->children.rbegin(); it != raw_root->children.rend();
             ++it) {
            pending.emplace_back(*it, npos_index);
        }
        while (!pending.empty()) {
            auto [node, parent] = std::move(pending.back());
            pending.pop_back();
            if (node == nullptr) continue;
            std::size_t child_parent = parent;
            if (node->type == flatworm::NodeType::Element) {
                if (elements.size() >= max_render_elements) {
                    throw Error(ErrorCode::ResourceLimit, "render element budget");
                }
                const auto index = elements.size();
                elements.push_back(std::make_shared<Element>(node));
                parent_index.push_back(parent);
                child_parent = index;
            }
            for (auto it = node->children.rbegin(); it != node->children.rend(); ++it) {
                pending.emplace_back(*it, child_parent);
            }
        }
    }

    // The sheets: user agent first, then author `<style>` elements in document
    // order. Origin is recorded per rule so the cascade can order them.
    std::vector<CssRule> rules;
    std::vector<int> origins;
    const auto append = [&rules, &origins](std::vector<CssRule> sheet, int origin) {
        for (auto& rule : sheet) {
            rule.order = rules.size();
            rules.push_back(std::move(rule));
            origins.push_back(origin);
        }
    };
    append(parse_stylesheet(user_agent_stylesheet()),
           static_cast<int>(Origin::UserAgent));
    bool style_limited = false;
    const auto author_text = collect_style_text(*raw_root, style_limited);
    if (!author_text.empty()) {
        append(parse_stylesheet(author_text),
               static_cast<int>(Origin::Author));
    }

    const auto sheets = bucket(rules, origins);
    data->elements = std::move(elements);
    for (std::size_t i = 0; i < data->elements.size(); ++i)
        data->index.emplace(data->elements[i]->node().get(), i);
    data->styles.assign(data->elements.size(), ComputedStyle{});
    data->root = initial_style(context);

    const auto initial = initial_style(context);
    std::size_t order_counter = 0;
    std::size_t match_budget = 2000000;
    std::size_t inline_bytes = 0;
    for (std::size_t i = 0; i < data->elements.size(); ++i) {
        const auto& element = *data->elements[i];
        const std::size_t parent = parent_index[i];
        // Document order guarantees a parent precedes its children.
        const ComputedStyle& parent_style =
            parent == npos_index ? initial : data->styles[parent];

        DeclaredMap declared;
        for (const auto& ref : sheets.universal) {
            if (match_budget-- == 0) throw Error(ErrorCode::ResourceLimit, "render selector budget");
            bool matches = false;
            try { matches = ref.rule->selector == "*" || element.matches(ref.rule->selector); }
            catch (const Error& error) { if (error.code() != ErrorCode::ParseError) throw; }
            if (matches) {
                apply(ref, declared, order_counter);
            }
        }
        for (const auto& [key, refs] : sheets.keyed) {
            if (match_budget-- == 0) throw Error(ErrorCode::ResourceLimit, "render selector budget");
            if (!bucket_compatible(key, element)) continue;
            for (const auto& ref : refs) {
                if (match_budget-- == 0) throw Error(ErrorCode::ResourceLimit, "render selector budget");
                bool matches = false;
                try { matches = element.matches(ref.rule->selector); }
                catch (const Error& error) { if (error.code() != ErrorCode::ParseError) throw; }
                if (matches) {
                    apply(ref, declared, order_counter);
                }
            }
        }
        if (element.has_attribute("style")) {
            order_counter = (rules.size() + 1) * (max_render_declarations + 1);
            const auto inline_style = element.attribute("style");
            inline_bytes += inline_style.size();
            if (inline_bytes > max_render_stylesheet_bytes)
                throw Error(ErrorCode::ResourceLimit, "inline stylesheet budget");
            apply_inline(inline_style, declared, order_counter);
        }

        DeclaredMap expanded;
        expand_shorthands(declared, expanded);

        // Only inherited (and text-decoration) declarations are carried in; the
        // rest start from their initial values inside `compute`.
        DeclaredMap carried;
        for (const auto& [property, value] : expanded) {
            if (inherits(property) ||
                propagates_to_descendants(property)) {
                carried[property] = value;
            }
        }
        data->styles[i] = compute(element, expanded, parent_style, context,
                                          parent == npos_index);
        auto& source = data->styles[i].replaced_source;
        if (!source.empty()) {
            try { source = resolve_url(document.base_url(), source); }
            catch (const Error&) { source.clear(); }
        }
        if (parent == npos_index) data->root = data->styles[i];
    }
    data->limited = style_limited;
    return ComputedStyles(std::move(data));
}

const ComputedStyle* ComputedStyles::find(const Element& element) const {
    const auto found = data_->index.find(element.node().get());
    return found == data_->index.end() ? nullptr : &data_->styles[found->second];
}

const ComputedStyle& ComputedStyles::at(const Element& element) const {
    const auto* style = find(element);
    return style != nullptr ? *style : data_->root;
}

}  // namespace prowsetk
