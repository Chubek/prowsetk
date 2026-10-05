// Bounded normal-flow layout. Styles own the cascade; paint owns encoding.
#include "core/render/render_internal.hpp"
#include "core/render/text_metrics.hpp"
#include "flatworm/dom_internal.hpp"
#include <prowsetk/error.hpp>
#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace prowsetk::detail {
namespace {
double bounded(double value) {
    return std::isfinite(value) ? std::clamp(value, -max_render_content_pixels,
                                          max_render_content_pixels) : 0.0;
}
bool block(Display d) {
    return d == Display::Block || d == Display::ListItem || d == Display::Table ||
           d == Display::TableRow || d == Display::TableCell;
}
class Layout {
public:
    Layout(const ComputedStyles& styles, const StyleContext& context,
           const TextMeasurer& measurer) : context_(context), measurer_(measurer) {
        for (const auto& element : styles.elements())
            styles_.emplace(element->node().get(), std::pair{element, &styles.at(*element)});
    }
    LayoutResult run(const Document& document, const ComputedStyle& root_style) {
        Flow flow{0, 0, context_.viewport_width};
        if (document.raw_root()) children(*document.raw_root(), {}, root_style, flow, {}, 1, 0);
        flush(flow);
        result_.bounds = {0, 0, context_.viewport_width, flow.y};
        return std::move(result_);
    }
private:
    struct Flow {
        double left, y, width;
        double used = 0, line_height = 0, previous_margin = 0;
        bool pending_space = false;
    };
    double length(const Length& l, const ComputedStyle& s, double base) const {
        if (l.is_auto) return 0;
        double scale = 1;
        switch (l.unit) {
        case LengthUnit::Auto: return 0;
        case LengthUnit::Px: break;
        case LengthUnit::Em: scale = s.font.size; break;
        case LengthUnit::Rem: scale = context_.root_font_size; break;
        case LengthUnit::Percent: scale = base / 100; break;
        case LengthUnit::Vw: scale = context_.viewport_width / 100; break;
        case LengthUnit::Vh: scale = context_.viewport_height / 100; break;
        case LengthUnit::Vmin: scale = std::min(context_.viewport_width, context_.viewport_height) / 100; break;
        case LengthUnit::Vmax: scale = std::max(context_.viewport_width, context_.viewport_height) / 100; break;
        }
        return bounded(l.value * scale);
    }
    double size(double v, const Length& low, const Length& high,
                const ComputedStyle& s, double base) const {
        if (!high.is_auto) v = std::min(v, length(high, s, base));
        if (!low.is_auto) v = std::max(v, length(low, s, base));
        return std::max(0.0, bounded(v));
    }
    bool budget() {
        if (result_.order.size() < max_render_paint_items / 2) return true;
        result_.limited = true;
        return false;
    }
    static void flush(Flow& f) {
        f.y = bounded(f.y + f.line_height);
        f.used = f.line_height = 0;
        f.pending_space = false;
    }
    void text(std::string_view source, const std::shared_ptr<Element>& owner,
              const ComputedStyle& s, Flow& f, std::optional<Rect> clip, double opacity) {
        const bool preserve = s.white_space == WhiteSpace::Pre || s.white_space == WhiteSpace::PreWrap;
        const bool newline = preserve || s.white_space == WhiteSpace::PreLine;
        const bool wrap = s.white_space != WhiteSpace::Pre && s.white_space != WhiteSpace::NoWrap;
        const auto metrics = measurer_.metrics(s.font);
        const double height = resolve_line_height(s.font.size, s.line_height, metrics);
        std::size_t i = 0;
        while (i < source.size() && budget()) {
            const char c = source[i];
            if (newline && (c == '\n' || c == '\r')) {
                ++i;
                if (c == '\r' && i < source.size() && source[i] == '\n') ++i;
                f.line_height = std::max(f.line_height, height);
                flush(f);
                continue;
            }
            if (!preserve && (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f')) {
                f.pending_space = true;
                ++i;
                continue;
            }
            const auto start = i++;
            while (i < source.size() && source[i] != ' ' && source[i] != '\t' &&
                   source[i] != '\r' && source[i] != '\n' && source[i] != '\f') ++i;
            auto word = source.substr(start, i - start);
            double gap = f.pending_space && f.used > 0 ? measurer_.measure(" ", s.font) : 0;
            f.pending_space = false;
            const double width = measurer_.measure(word, s.font);
            if (wrap && f.used > 0 && f.used + gap + width > f.width) { flush(f); gap = 0; }
            f.used += gap;
            const auto spans = break_text(word, s.font, wrap ? std::max(1.0, f.width - f.used) :
                                          max_render_content_pixels,
                                          wrap ? WhiteSpace::Normal : WhiteSpace::NoWrap, measurer_);
            for (const auto& [begin, end] : spans) {
                if (!budget()) break;
                if (begin != 0) flush(f);
                const auto part = word.substr(begin, end - begin);
                const double advance = measurer_.measure(part, s.font);
                if (s.visibility == Visibility::Visible) {
                    TextFragment t;
                    t.element = owner; t.rect = {f.left + f.used, f.y, advance, height};
                    t.text = std::string(part); t.font = s.font; t.baseline = metrics.ascent;
                    t.color = s.color; t.opacity = opacity; t.clip = clip;
                    t.underline = s.underline; t.line_through = s.line_through;
                    result_.order.push_back({PaintKind::Text, result_.texts.size()});
                    result_.texts.push_back(std::move(t));
                }
                f.used = bounded(f.used + advance);
                f.line_height = std::max(f.line_height, height);
            }
        }
    }
    void children(const flatworm::Node& node, const std::shared_ptr<Element>& owner,
                  const ComputedStyle& s, Flow& flow, std::optional<Rect> clip,
                  double opacity, std::size_t depth) {
        bool summary_seen = false;
        const auto saved_index = list_index_;
        const bool list = node.name == "ol" || node.name == "ul";
        if (list) list_index_ = 0;
        for (const auto& child : node.children) {
            if (!budget()) break;
            if (node.name == "details" && owner && !owner->has_attribute("open")) {
                if (child->name != "summary" || summary_seen) continue;
                summary_seen = true;
            }
            if (child->type == flatworm::NodeType::Text) text(child->text, owner, s, flow, clip, opacity);
            else if (child->type == flatworm::NodeType::Element) element(*child, flow, clip, opacity, depth + 1);
        }
        if (list) list_index_ = saved_index;
    }
    void element(const flatworm::Node& node, Flow& outer, std::optional<Rect> clip,
                 double parent_opacity, std::size_t depth) {
        if (depth > max_render_depth) throw Error(ErrorCode::ResourceLimit, "render depth limit");
        const auto found = styles_.find(&node);
        if (found == styles_.end()) return;
        const auto& [owner, ptr] = found->second;
        const auto& s = *ptr;
        if (s.display == Display::None) return;
        const double opacity = parent_opacity * s.opacity;
        const auto marker_index = s.display == Display::ListItem ? ++list_index_ : 0;
        if (node.name == "br") {
            outer.line_height = std::max(outer.line_height, measurer_.metrics(s.font).line_height);
            flush(outer); return;
        }
        const bool control = s.control != ControlKindHint::None;
        const bool atomic = control || s.is_replaced || s.display == Display::InlineBlock;
        if (!block(s.display) && !atomic) {
            // Inline descendants share the line cursor, including whitespace.
            children(node, owner, s, outer, clip, opacity, depth);
            return;
        }
        const bool in_block = block(s.display);
        if (in_block) flush(outer);
        const double horizontal = s.padding.horizontal() + s.border_width[1] + s.border_width[3];
        const double vertical = s.padding.vertical() + s.border_width[0] + s.border_width[2];
        double width = s.width.is_auto ? std::max(0.0, outer.width - s.margin.horizontal() - horizontal)
                                     : length(s.width, s, outer.width);
        if (atomic && s.width.is_auto) {
            const auto label = control ? (s.control_value.empty() ? owner->text() : s.control_value) :
                               (s.is_replaced ? s.replaced_alt : owner->text());
            width = std::min(width, std::max(s.font.size, measurer_.measure(label, s.font)));
        }
        width = size(width, s.min_width, s.max_width, s, outer.width);
        if (!in_block && outer.used > 0 && outer.used + width + horizontal + s.margin.horizontal() > outer.width)
            flush(outer);
        const double margin = in_block ? std::max(s.margin.top, outer.previous_margin) : s.margin.top;
        const double x = bounded(outer.left + outer.used + s.margin.left +
                                (s.position == Position::Relative ? length(s.offset_x, s, outer.width) : 0));
        const double y = bounded(outer.y + margin +
                                (s.position == Position::Relative ? length(s.offset_y, s, outer.width) : 0));
        const double cx = x + s.padding.left + s.border_width[3];
        const double cy = y + s.padding.top + s.border_width[0];
        const std::size_t box_index = result_.boxes.size();
        BoxFragment box;
        box.element = owner; box.border_box = {x, y, width + horizontal, 0};
        box.padding = s.padding; box.background = s.background_color; box.border_style = s.border_style;
        box.opacity = s.visibility == Visibility::Visible ? opacity : 0; box.clip = clip;
        std::copy_n(s.border_width, 4, box.border_widths);
        std::copy_n(s.border_color, 4, box.border_colors);
        result_.order.push_back({PaintKind::Background, box_index});
        result_.boxes.push_back(box);
        const auto content_start = result_.order.size();
        Flow inner{cx, cy, width};
        if (control || s.is_replaced) {
            inner.y += s.height.is_auto ? measurer_.metrics(s.font).line_height : length(s.height, s, context_.viewport_height);
        } else { children(node, owner, s, inner, clip, opacity, depth); flush(inner); }
        double height = s.height.is_auto ? inner.y - cy : length(s.height, s, context_.viewport_height);
        height = size(height, s.min_height, s.max_height, s, context_.viewport_height);
        result_.boxes[box_index].border_box.height = height + vertical;
        if (s.visibility == Visibility::Visible && control) {
            ControlFragment c;
            c.element = owner; c.rect = {cx, cy, width, height};
            c.kind = static_cast<ControlKind>(s.control); c.font = s.font;
            c.value = s.control == ControlKindHint::PasswordField ? "[redacted]" : s.control_value;
            if ((s.control == ControlKindHint::Button || s.control == ControlKindHint::Submit) && node.name == "button") c.value = owner->text();
            c.placeholder = s.control_placeholder; c.checked = s.control_checked; c.disabled = s.control_disabled;
            c.read_only = s.control_read_only; c.foreground = s.color; c.opacity = opacity; c.clip = clip;
            result_.order.push_back({PaintKind::Control, result_.controls.size()});
            result_.controls.push_back(std::move(c));
        } else if (s.visibility == Visibility::Visible && s.is_replaced) {
            result_.order.push_back({PaintKind::Image, result_.images.size()});
            result_.images.push_back({owner, {cx, cy, width, height}, s.replaced_source,
                                      s.replaced_alt, s.object_fit, opacity, clip});
        }
        if (s.visibility == Visibility::Visible && s.display == Display::ListItem && s.list_style_type != ListStyleType::None) {
            MarkerFragment marker;
            marker.element = owner; marker.rect = {x - s.font.size, cy, s.font.size, s.font.size * 1.2};
            marker.text = s.list_style_type == ListStyleType::Decimal ? std::to_string(marker_index) + "." : "•";
            marker.font = s.font; marker.color = s.color; marker.opacity = opacity; marker.clip = clip;
            result_.order.push_back({PaintKind::Marker, result_.markers.size()});
            result_.markers.push_back(std::move(marker));
        }
        if (s.overflow == Overflow::Hidden) {
            const Rect own{cx, cy, width, height};
            for (std::size_t i = content_start; i < result_.order.size(); ++i) {
                const auto entry = result_.order[i];
                const auto apply = [&](auto& fragment) {
                    fragment.clip = fragment.clip ? fragment.clip->intersect(own) : own;
                };
                switch (entry.kind) {
                case PaintKind::Background: apply(result_.boxes[entry.index]); break;
                case PaintKind::Text: apply(result_.texts[entry.index]); break;
                case PaintKind::Image: apply(result_.images[entry.index]); break;
                case PaintKind::Control: apply(result_.controls[entry.index]); break;
                case PaintKind::Marker: apply(result_.markers[entry.index]); break;
                case PaintKind::Border: break;
                }
            }
        }
        if (in_block) { outer.y = bounded(outer.y + margin + height + vertical); outer.previous_margin = s.margin.bottom; }
        else { outer.used = bounded(outer.used + width + horizontal + s.margin.horizontal());
               outer.line_height = std::max(outer.line_height, height + vertical + s.margin.vertical()); }
    }
    const StyleContext& context_;
    const TextMeasurer& measurer_;
    std::unordered_map<const flatworm::Node*, std::pair<std::shared_ptr<Element>, const ComputedStyle*>> styles_;
    LayoutResult result_;
    std::size_t list_index_ = 0;
};
}
LayoutResult layout_document(const Document& document, const ComputedStyles& styles,
                             const StyleContext& context, const TextMeasurer& measurer,
                             double pixel_ratio) {
    (void)pixel_ratio; // CSS pixel coordinates; scaling belongs to the consumer.
    return Layout(styles, context, measurer).run(document, styles.root());
}
}
