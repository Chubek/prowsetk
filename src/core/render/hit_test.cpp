#include "core/render/render_internal.hpp"
#include <cmath>

namespace prowsetk::detail {
bool is_interactive_tag(std::string_view tag) {
    return tag == "a" || tag == "button" || tag == "input" || tag == "select" ||
           tag == "textarea" || tag == "summary";
}
}
namespace prowsetk {
bool is_interactive_element(const Element& element) {
    if (element.has_attribute("disabled") || element.attribute("aria-disabled") == "true") return false;
    if (element.tag_name() == "a") return element.has_attribute("href");
    if (detail::is_interactive_tag(element.tag_name())) return true;
    return element.has_attribute("onclick") || element.has_attribute("onpointerdown") ||
           element.has_attribute("onmousedown");
}
HitResult hit_test(const PaintList& paint, double x, double y) {
    if (!std::isfinite(x) || !std::isfinite(y)) return {};
    for (auto it = paint.items.rbegin(); it != paint.items.rend(); ++it) {
        if (!it->element || it->opacity <= 0 || !it->rect.contains(x, y) ||
            (it->clip && !it->clip->contains(x, y))) continue;
        HitResult result;
        result.element = it->element; result.rect = it->rect;
        result.kind = it->kind == PaintKind::Text ? HitKind::Text : HitKind::Element;
        auto node = it->element;
        for (std::size_t depth = 0; node && depth <= max_render_depth; ++depth, node = node->parent()) {
            if (node->has_attribute("disabled") || node->attribute("aria-disabled") == "true") break;
            if (is_interactive_element(*node)) {
                result.interactive = node;
                result.kind = it->kind == PaintKind::Control ? HitKind::Control : HitKind::Interactive;
                break;
            }
        }
        return result;
    }
    return {};
}
}
