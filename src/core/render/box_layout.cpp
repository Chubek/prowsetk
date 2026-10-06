// Bounded normal-flow layout. Styles own the cascade; paint owns encoding.
#include "core/render/render_internal.hpp"
#include "core/render/text_metrics.hpp"
#include "core/render/flex_layout.hpp"
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
           d == Display::TableRow || d == Display::TableCell || d == Display::Flex;
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
        TextAlign align = TextAlign::Start;
        std::vector<std::pair<std::size_t, std::size_t>> fragments{};
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
                const ComputedStyle& s, double base, double decoration = 0) const {
        const double subtract = s.box_sizing == BoxSizing::BorderBox ? decoration : 0;
        if (!high.is_auto) v = std::min(v, std::max(0.0, length(high, s, base) - subtract));
        if (!low.is_auto) v = std::max(v, std::max(0.0, length(low, s, base) - subtract));
        return std::max(0.0, bounded(v));
    }
    bool budget() {
        if (result_.order.size() < max_render_paint_items / 2) return true;
        result_.limited = true;
        return false;
    }
    template<class Visitor>
    void visit(std::size_t begin, std::size_t end, Visitor visitor) {
        for (auto i = begin; i < end; ++i) {
            const auto entry = result_.order[i];
            switch (entry.kind) {
            case PaintKind::Background: {
                auto& f = result_.boxes[entry.index]; visitor(f.border_box, f.clip); break;
            }
            case PaintKind::Text: { auto& f = result_.texts[entry.index]; visitor(f.rect, f.clip); break; }
            case PaintKind::Image: { auto& f = result_.images[entry.index]; visitor(f.rect, f.clip); break; }
            case PaintKind::Control: { auto& f = result_.controls[entry.index]; visitor(f.rect, f.clip); break; }
            case PaintKind::Marker: { auto& f = result_.markers[entry.index]; visitor(f.rect, f.clip); break; }
            case PaintKind::Border: break;
            }
        }
    }
    void translate(std::size_t begin, std::size_t end, double dx, double dy) {
        visit(begin, end, [&](Rect& rect, std::optional<Rect>& clip) {
            rect.x = bounded(rect.x + dx); rect.y = bounded(rect.y + dy);
            if (clip) { clip->x = bounded(clip->x + dx); clip->y = bounded(clip->y + dy); }
        });
    }
    void constrain(std::size_t begin, std::size_t end, const Rect& bounds) {
        visit(begin, end, [&](Rect&, std::optional<Rect>& clip) {
            clip = clip ? clip->intersect(bounds) : bounds;
        });
    }
    void flush(Flow& f) {
        const double free = f.width - f.used;
        const double dx = f.align == TextAlign::Center ? free / 2 : f.align == TextAlign::End ? free : 0;
        if (dx != 0) for (const auto& [begin, end] : f.fragments) translate(begin, end, dx, 0);
        f.fragments.clear();
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
                    f.fragments.emplace_back(result_.order.size() - 1, result_.order.size());
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
    void flex_children(const flatworm::Node& node, const std::shared_ptr<Element>& owner,
                       const ComputedStyle& s, Flow& flow, double opacity,
                       std::size_t depth, std::optional<double> definite_height) {
        struct Item {
            const flatworm::Node* node;
            const ComputedStyle* style;
            std::shared_ptr<Element> element;
            std::size_t begin = 0, end = 0, box = 0;
            double height = 0;
        };
        std::vector<Item> items;
        bool summary_seen = false;
        for (const auto& child : node.children) {
            if (node.name == "details" && owner && !owner->has_attribute("open")) {
                if (child->name != "summary" || summary_seen) continue;
                summary_seen = true;
            }
            if (child->type == flatworm::NodeType::Text) {
                if (child->text.find_first_not_of(" \t\r\n\f") != std::string::npos)
                    items.push_back({child.get(), &s, owner});
            } else if (child->type == flatworm::NodeType::Element) {
                const auto found = styles_.find(child.get());
                if (found != styles_.end() && found->second.second->display != Display::None)
                    items.push_back({child.get(), found->second.second, found->second.first});
            }
            if (items.size() > max_render_elements) throw Error(ErrorCode::ResourceLimit, "render flex item budget");
        }
        const auto item_order = [](const Item& item) {
            return item.node->type == flatworm::NodeType::Text ? 0 : item.style->order;
        };
        std::stable_sort(items.begin(), items.end(), [&](const Item& a, const Item& b) { return item_order(a) < item_order(b); });
        std::vector<FlexMeasure> measures;
        measures.reserve(items.size());
        for (const auto& item : items) {
            const auto& style = *item.style;
            const bool anonymous = item.node->type == flatworm::NodeType::Text;
            FlexMeasure measure;
            const double decoration = anonymous ? 0 : style.padding.horizontal() + style.border_width[1] + style.border_width[3];
            const double subtract = style.box_sizing == BoxSizing::BorderBox ? decoration : 0;
            const auto basis = anonymous ? Length::auto_length() : style.flex_basis.is_auto ? style.width : style.flex_basis;
            if (basis.is_auto) {
                const auto label = anonymous ? item.node->text : style.control != ControlKindHint::None ?
                    (item.node->name == "button" ? item.element->text() : style.control_value) :
                    style.is_replaced ? style.replaced_alt : item.element->text();
                if (label.size() > intrinsic_bytes_) throw Error(ErrorCode::ResourceLimit, "render intrinsic text budget");
                intrinsic_bytes_ -= label.size();
                measure.basis = std::max(0.0, bounded(measurer_.measure(label, style.font)));
            } else measure.basis = std::max(0.0, length(basis, style, flow.width) - subtract);
            if (!anonymous) {
                measure.minimum = style.min_width.is_auto ? 0 : std::max(0.0, length(style.min_width, style, flow.width) - subtract);
                measure.maximum = style.max_width.is_auto ? max_render_content_pixels :
                    std::max(measure.minimum, length(style.max_width, style, flow.width) - subtract);
                measure.fixed = decoration + style.margin.horizontal();
                measure.margin_left = style.margin.left; measure.margin_right = style.margin.right;
                measure.auto_left = style.margin_left_auto; measure.auto_right = style.margin_right_auto;
                measure.grow = style.flex_grow; measure.shrink = style.flex_shrink;
            }
            measures.push_back(measure);
        }
        const double column_gap = std::max(0.0, length(s.column_gap, s, flow.width));
        const double row_gap = std::max(0.0, length(s.row_gap, s, flow.width));
        const auto lines = arrange_flex_rows(measures, flow.width, column_gap,
            s.flex_wrap, s.flex_direction, s.justify_content, flex_work_);
        for (const auto line : lines) {
            double line_height = s.flex_wrap == FlexWrap::NoWrap ? definite_height.value_or(0) : 0;
            for (auto i = line.begin; i < line.end; ++i) {
                if (!budget()) break;
                auto& item = items[i];
                item.begin = result_.order.size(); item.box = result_.boxes.size();
                Flow inner{0, 0, measures[i].width};
                if (item.node->type == flatworm::NodeType::Text) {
                    text(item.node->text, owner, s, inner, {}, opacity); flush(inner);
                } else {
                    auto style = *item.style;
                    if (!block(style.display)) style.display = Display::Block;
                    style.width = Length::px(measures[i].width);
                    // Only width is overridden; convert height constraints too
                    // before switching the item to content-box coordinates.
                    if (style.box_sizing == BoxSizing::BorderBox) {
                        const double vertical = style.padding.vertical() + style.border_width[0] + style.border_width[2];
                        for (auto* value : {&style.height, &style.min_height, &style.max_height})
                            if (!value->is_auto) *value = Length::px(std::max(0.0, length(*value, style, context_.viewport_height) - vertical));
                    }
                    style.box_sizing = BoxSizing::ContentBox;
                    style.min_width = style.max_width = Length::auto_length();
                    style.margin = {};
                    style.margin_left_auto = style.margin_right_auto = false;
                    element(*item.node, inner, {}, opacity, depth + 1, &style, true);
                }
                item.end = result_.order.size(); item.height = inner.y;
                const double margins = item.node->type == flatworm::NodeType::Text ? 0 : item.style->margin.vertical();
                if (s.flex_wrap != FlexWrap::NoWrap || !definite_height)
                    line_height = std::max(line_height, item.height + margins);
            }
            for (auto i = line.begin; i < line.end; ++i) {
                auto& item = items[i];
                if (item.begin == item.end) continue;
                const auto& style = *item.style;
                const bool anonymous = item.node->type == flatworm::NodeType::Text;
                const double top = anonymous ? 0 : style.margin.top;
                const double margins = anonymous ? 0 : style.margin.vertical();
                const auto align = anonymous ? s.align_items : style.align_self.value_or(s.align_items);
                if (!anonymous && align == AlignItems::Stretch && style.height.is_auto && item.box < result_.boxes.size()) {
                    const double vertical = style.padding.vertical() + style.border_width[0] + style.border_width[2];
                    const double content_height = size(std::max(0.0, line_height - margins - vertical),
                        style.min_height, style.max_height, style, context_.viewport_height, vertical);
                    item.height = content_height + vertical;
                    result_.boxes[item.box].border_box.height = item.height;
                    // Replaced content fills the stretched content box.
                    if (style.is_replaced || style.control != ControlKindHint::None)
                        for (auto j = item.begin; j < item.end; ++j) {
                            const auto entry = result_.order[j];
                            if (entry.kind == PaintKind::Image) result_.images[entry.index].rect.height = content_height;
                            if (entry.kind == PaintKind::Control) result_.controls[entry.index].rect.height = content_height;
                        }
                }
                const double free = line_height - item.height - margins;
                const double dy = top + (align == AlignItems::Center ? free / 2 : align == AlignItems::End ? free : 0);
                translate(item.begin, item.end, bounded(flow.left + measures[i].x), bounded(flow.y + dy));
                if (!anonymous && style.overflow == Overflow::Hidden && item.box < result_.boxes.size()) {
                    const auto& box = result_.boxes[item.box].border_box;
                    constrain(item.begin + 1, item.end, {box.x + style.padding.left + style.border_width[3],
                        box.y + style.padding.top + style.border_width[0], measures[i].width,
                        std::max(0.0, item.height - style.padding.vertical() - style.border_width[0] - style.border_width[2])});
                }
            }
            flow.y = bounded(flow.y + line_height + (line.end < items.size() ? row_gap : 0));
        }
    }
    void element(const flatworm::Node& node, Flow& outer, std::optional<Rect> clip,
                 double parent_opacity, std::size_t depth, const ComputedStyle* override_style = nullptr,
                 bool defer_clip = false) {
        if (depth > max_render_depth) throw Error(ErrorCode::ResourceLimit, "render depth limit");
        const auto found = styles_.find(&node);
        if (found == styles_.end()) return;
        const auto& [owner, ptr] = found->second;
        const auto& s = override_style ? *override_style : *ptr;
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
                                     : std::max(0.0, length(s.width, s, outer.width) -
                                         (s.box_sizing == BoxSizing::BorderBox ? horizontal : 0));
        if (atomic && s.width.is_auto) {
            const auto label = control ? (s.control_value.empty() ? owner->text() : s.control_value) :
                               (s.is_replaced ? s.replaced_alt : owner->text());
            width = std::min(width, std::max(s.font.size, measurer_.measure(label, s.font)));
        }
        width = size(width, s.min_width, s.max_width, s, outer.width, horizontal);
        if (!in_block && outer.used > 0 && outer.used + width + horizontal + s.margin.horizontal() > outer.width)
            flush(outer);
        const double margin = in_block ? std::max(s.margin.top, outer.previous_margin) : s.margin.top;
        const double free = std::max(0.0, outer.width - width - horizontal - s.margin.horizontal());
        const double auto_left = in_block && s.margin_left_auto ? free / (s.margin_right_auto ? 2 : 1) : 0;
        const double x = bounded(outer.left + outer.used + s.margin.left + auto_left +
                                (s.position == Position::Relative ? length(s.offset_x, s, outer.width) : 0));
        const double y = bounded(outer.y + margin +
                                (s.position == Position::Relative ? length(s.offset_y, s, outer.width) : 0));
        const double cx = x + s.padding.left + s.border_width[3];
        const double cy = y + s.padding.top + s.border_width[0];
        const std::size_t box_index = result_.boxes.size();
        BoxFragment box;
        box.element = owner; box.border_box = {x, y, width + horizontal, 0};
        box.padding = s.padding; box.background = s.background_color; box.border_style = s.border_style;
        box.opacity = s.visibility == Visibility::Visible ? opacity : 0;
        std::copy_n(s.border_width, 4, box.border_widths);
        std::copy_n(s.border_color, 4, box.border_colors);
        const auto box_start = result_.order.size();
        result_.order.push_back({PaintKind::Background, box_index});
        result_.boxes.push_back(box);
        const auto content_start = result_.order.size();
        Flow inner{cx, cy, width};
        inner.align = s.text_align;
        const double specified_height = s.height.is_auto ? 0 : std::max(0.0,
            length(s.height, s, context_.viewport_height) - (s.box_sizing == BoxSizing::BorderBox ? vertical : 0));
        if (control || s.is_replaced) {
            inner.y += s.height.is_auto ? measurer_.metrics(s.font).line_height : specified_height;
        } else if (s.display == Display::Flex) {
            flex_children(node, owner, s, inner, opacity, depth,
                s.height.is_auto ? std::nullopt : std::optional{size(specified_height,
                    s.min_height, s.max_height, s, context_.viewport_height, vertical)});
        } else { children(node, owner, s, inner, {}, opacity, depth); flush(inner); }
        double height = s.height.is_auto ? inner.y - cy : specified_height;
        height = size(height, s.min_height, s.max_height, s, context_.viewport_height, vertical);
        result_.boxes[box_index].border_box.height = height + vertical;
        if (s.visibility == Visibility::Visible && control) {
            ControlFragment c;
            c.element = owner; c.rect = {cx, cy, width, height};
            c.kind = static_cast<ControlKind>(s.control); c.font = s.font;
            c.value = s.control == ControlKindHint::PasswordField ? "[redacted]" : s.control_value;
            if ((s.control == ControlKindHint::Button || s.control == ControlKindHint::Submit) && node.name == "button") c.value = owner->text();
            c.placeholder = s.control_placeholder; c.checked = s.control_checked; c.disabled = s.control_disabled;
            c.read_only = s.control_read_only; c.foreground = s.color; c.opacity = opacity;
            result_.order.push_back({PaintKind::Control, result_.controls.size()});
            result_.controls.push_back(std::move(c));
        } else if (s.visibility == Visibility::Visible && s.is_replaced) {
            result_.order.push_back({PaintKind::Image, result_.images.size()});
            result_.images.push_back({owner, {cx, cy, width, height}, s.replaced_source,
                                      s.replaced_alt, s.object_fit, opacity, {}});
        }
        if (s.visibility == Visibility::Visible && s.display == Display::ListItem && s.list_style_type != ListStyleType::None) {
            MarkerFragment marker;
            marker.element = owner; marker.rect = {x - s.font.size, cy, s.font.size, s.font.size * 1.2};
            marker.text = s.list_style_type == ListStyleType::Decimal ? std::to_string(marker_index) + "." : "•";
            marker.font = s.font; marker.color = s.color; marker.opacity = opacity;
            result_.order.push_back({PaintKind::Marker, result_.markers.size()});
            result_.markers.push_back(std::move(marker));
        }
        if (s.overflow == Overflow::Hidden && !defer_clip) {
            const Rect own{cx, cy, width, height};
            constrain(content_start, result_.order.size(), own);
        }
        if (clip) constrain(box_start, result_.order.size(), *clip);
        if (in_block) { outer.y = bounded(outer.y + margin + height + vertical); outer.previous_margin = s.margin.bottom; }
        else { outer.fragments.emplace_back(box_start, result_.order.size());
               outer.used = bounded(outer.used + width + horizontal + s.margin.horizontal());
               outer.line_height = std::max(outer.line_height, height + vertical + s.margin.vertical()); }
    }
    const StyleContext& context_;
    const TextMeasurer& measurer_;
    std::unordered_map<const flatworm::Node*, std::pair<std::shared_ptr<Element>, const ComputedStyle*>> styles_;
    LayoutResult result_;
    std::size_t list_index_ = 0;
    std::size_t flex_work_ = 2000000;
    std::size_t intrinsic_bytes_ = 16u * 1024u * 1024u;
};
}
LayoutResult layout_document(const Document& document, const ComputedStyles& styles,
                             const StyleContext& context, const TextMeasurer& measurer,
                             double pixel_ratio) {
    (void)pixel_ratio; // CSS pixel coordinates; scaling belongs to the consumer.
    return Layout(styles, context, measurer).run(document, styles.root());
}
}
