#include "core/render/render_internal.hpp"
#include <prowsetk/error.hpp>
#include <algorithm>
#include <cmath>
#include "flatworm/dom_internal.hpp"

namespace prowsetk {
RenderedPage render_document(const Document& document, const RenderOptions& options) {
    const auto valid = [](double value, double maximum) {
        return std::isfinite(value) && value > 0 && value <= maximum;
    };
    if (!valid(options.viewport_width, max_render_viewport_dimension) ||
        !valid(options.viewport_height, max_render_viewport_dimension) ||
        !valid(options.root_font_size, 512) || !valid(options.pixel_ratio, 16))
        throw Error(ErrorCode::InvalidArgument, "invalid render dimensions");
    std::vector<std::pair<const flatworm::Node*, std::size_t>> pending;
    if (document.raw_root()) pending.emplace_back(document.raw_root().get(), 0);
    std::size_t text_bytes = 0, nodes = 0;
    while (!pending.empty()) {
        const auto [node, depth] = pending.back(); pending.pop_back();
        text_bytes += node->text.size();
        for (const auto& attribute : node->attributes)
            text_bytes += attribute.name.size() + attribute.value.size();
        if (++nodes > max_render_elements * 4 || depth > max_render_depth || text_bytes > 16u * 1024u * 1024u)
            throw Error(ErrorCode::ResourceLimit, "render document budget");
        for (const auto& child : node->children) pending.emplace_back(child.get(), depth + 1);
    }
    const auto styles = resolve_computed_styles(document, options.viewport_width,
                                                options.viewport_height, options.root_font_size);
    const detail::StyleContext style_context{options.viewport_width, options.viewport_height, options.root_font_size};
    const auto layout = detail::layout_document(document, styles, style_context,
        options.measurer ? *options.measurer : default_text_measurer(), options.pixel_ratio);
    detail::PaintContext context;
    context.images = options.images; context.loader = options.loader;
    context.max_images = options.max_images; context.paint_controls = options.paint_controls;
    const auto dropped = options.images ? options.images->dropped() : 0;
    RenderedPage result;
    result.paint = detail::paint_layout(layout, context);
    result.content_width = std::max(options.viewport_width, result.paint.bounds.right());
    result.content_height = std::max({options.viewport_height, result.paint.bounds.bottom(), layout.bounds.bottom()});
    result.limited = styles.limited() || layout.limited || context.limited ||
                     (options.images && options.images->dropped() != dropped);
    return result;
}
}
