#include "core/render/render_internal.hpp"
#include <algorithm>

namespace prowsetk::detail {
PaintList paint_layout(const LayoutResult& layout, PaintContext& context) {
    PaintList result;
    const auto emit = [&](PaintItem item) {
        if (result.items.size() >= max_render_paint_items) { context.limited = true; return; }
        if (item.opacity <= 0 || item.rect.empty()) return;
        const auto visible = item.clip ? item.rect.intersect(*item.clip) : item.rect;
        if (!visible.empty()) {
            const double left = std::min(result.bounds.x, visible.x);
            const double top = std::min(result.bounds.y, visible.y);
            const double right = std::max(result.bounds.right(), visible.right());
            const double bottom = std::max(result.bounds.bottom(), visible.bottom());
            result.bounds = {left, top, right - left, bottom - top};
        }
        result.items.push_back(std::move(item));
    };
    for (const auto entry : layout.order) {
        PaintItem item;
        item.kind = entry.kind;
        switch (entry.kind) {
        case PaintKind::Background: {
            const auto& f = layout.boxes[entry.index];
            item.element = f.element; item.rect = f.border_box; item.clip = f.clip;
            item.opacity = f.opacity; item.color = f.background; item.padding = f.padding;
            // Transparent boxes still participate in semantic hit testing.
            emit(item);
            if (f.border_style != BorderStyle::None) {
                item.kind = PaintKind::Border; item.border_style = f.border_style;
                item.color = f.border_colors[0];
                std::copy_n(f.border_widths, 4, item.border_width);
                emit(std::move(item));
            }
            continue;
        }
        case PaintKind::Text: {
            const auto& f = layout.texts[entry.index];
            item.element = f.element; item.rect = f.rect; item.clip = f.clip; item.opacity = f.opacity;
            item.text = f.text; item.font = f.font; item.color = f.color; item.baseline = f.baseline;
            item.align = f.align;
            break;
        }
        case PaintKind::Marker: {
            const auto& f = layout.markers[entry.index];
            item.element = f.element; item.rect = f.rect; item.clip = f.clip; item.opacity = f.opacity;
            item.text = f.text; item.font = f.font; item.color = f.color; item.baseline = f.font.size;
            break;
        }
        case PaintKind::Control: {
            const auto& f = layout.controls[entry.index];
            item.element = f.element; item.rect = f.rect; item.clip = f.clip; item.opacity = f.opacity;
            item.text = f.value; item.font = f.font; item.color = f.foreground;
            item.control = f.kind; item.checked = f.checked; item.disabled = f.disabled;
            item.read_only = f.read_only; item.placeholder = f.placeholder;
            item.baseline = f.font.size;
            if (!context.paint_controls) { item.kind = PaintKind::Text; item.control = ControlKind::None; }
            break;
        }
        case PaintKind::Image: {
            const auto& f = layout.images[entry.index];
            item.element = f.element; item.rect = f.rect; item.clip = f.clip; item.opacity = f.opacity;
            item.alt = f.alt; item.object_fit = f.object_fit;
            if (context.images && context.loader && context.images_decoded < std::min(context.max_images, max_render_images)) {
                ++context.images_decoded;
                item.bitmap = context.images->get(*context.loader, f.source);
            } else if (context.images && context.loader) context.limited = true;
            item.degraded = !item.bitmap;
            break;
        }
        case PaintKind::Border: continue;
        }
        emit(std::move(item));
    }
    return result;
}
}
