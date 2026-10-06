#include "core/render/flex_layout.hpp"
#include <prowsetk/error.hpp>
#include <algorithm>
#include <cmath>

namespace prowsetk::detail {
namespace {
void spend(std::size_t& remaining) {
    if (remaining == 0) throw Error(ErrorCode::ResourceLimit, "render flex work budget");
    --remaining;
}

void resolve_line(std::vector<FlexMeasure>& items, FlexLine line, double width,
                  double gap, FlexDirection direction, JustifyContent justify,
                  std::size_t& work) {
    const auto count = line.end - line.begin;
    const double gaps = gap * static_cast<double>(count - 1);
    double hypothetical = gaps, fixed = gaps;
    for (auto i = line.begin; i < line.end; ++i) {
        const auto& item = items[i];
        fixed += item.fixed;
        hypothetical += std::clamp(item.basis, item.minimum, item.maximum) + item.fixed;
    }
    const bool grow = hypothetical < width;
    std::vector<bool> frozen(count, false);
    std::vector<double> violations(count, 0);
    double initial_free = width - fixed;
    for (auto i = line.begin; i < line.end; ++i) {
        auto& item = items[i];
        const auto constrained = std::clamp(item.basis, item.minimum, item.maximum);
        frozen[i - line.begin] = (grow ? item.grow : item.shrink) == 0 ||
            (grow ? item.basis > constrained : item.basis < constrained);
        item.width = frozen[i - line.begin] ? constrained : item.basis;
        initial_free -= item.width;
    }
    for (;;) {
        double free = width - fixed, factor = 0, weight = 0;
        std::size_t active = 0;
        for (auto i = line.begin; i < line.end; ++i) {
            spend(work);
            const auto& item = items[i];
            free -= frozen[i - line.begin] ? item.width : item.basis;
            if (frozen[i - line.begin]) continue;
            ++active;
            factor += grow ? item.grow : item.shrink;
            weight += grow ? item.grow : item.shrink * item.basis;
        }
        if (!active) break;
        if (factor < 1 && std::abs(initial_free * factor) < std::abs(free)) free = initial_free * factor;
        double violation = 0;
        for (auto i = line.begin; i < line.end; ++i) {
            spend(work);
            if (frozen[i - line.begin]) continue;
            auto& item = items[i];
            const double share = weight > 0 ? (grow ? item.grow : item.shrink * item.basis) / weight : 0;
            const double target = item.basis + free * share;
            item.width = std::clamp(target, item.minimum, item.maximum);
            violations[i - line.begin] = item.width - target;
            violation += item.width - target;
        }
        const bool done = std::abs(violation) < 1e-7;
        for (auto i = line.begin; i < line.end; ++i) {
            spend(work);
            const double delta = violations[i - line.begin];
            if (done || (violation > 0 && delta > 0) || (violation < 0 && delta < 0)) frozen[i - line.begin] = true;
        }
    }
    double free = width - fixed;
    std::size_t auto_margins = 0;
    for (auto i = line.begin; i < line.end; ++i) {
        free -= items[i].width;
        auto_margins += static_cast<std::size_t>(items[i].auto_left) + static_cast<std::size_t>(items[i].auto_right);
    }
    const double auto_margin = auto_margins ? std::max(0.0, free) / static_cast<double>(auto_margins) : 0;
    if (auto_margins && free > 0) free = 0;
    double cursor = 0, extra_gap = 0;
    switch (justify) {
    case JustifyContent::End: cursor = free; break;
    case JustifyContent::Center: cursor = free / 2; break;
    case JustifyContent::SpaceBetween:
        if (count > 1) extra_gap = std::max(0.0, free) / static_cast<double>(count - 1);
        break;
    case JustifyContent::SpaceAround:
        extra_gap = std::max(0.0, free) / static_cast<double>(count); cursor = free < 0 ? free / 2 : extra_gap / 2; break;
    case JustifyContent::SpaceEvenly:
        extra_gap = std::max(0.0, free) / static_cast<double>(count + 1); cursor = free < 0 ? free / 2 : extra_gap; break;
    case JustifyContent::Start: break;
    }
    for (auto i = line.begin; i < line.end; ++i) {
        auto& item = items[i];
        const double left = item.auto_left ? auto_margin : item.margin_left;
        const double right = item.auto_right ? auto_margin : item.margin_right;
        const double border_width = item.width + item.fixed - item.margin_left - item.margin_right;
        item.x = direction == FlexDirection::Row ? cursor + left : width - cursor - right - border_width;
        cursor += border_width + left + right + gap + extra_gap;
    }
}
} // namespace

std::vector<FlexLine> arrange_flex_rows(std::vector<FlexMeasure>& items,
    double width, double gap, FlexWrap wrap, FlexDirection direction,
    JustifyContent justify, std::size_t& work_remaining) {
    std::vector<FlexLine> lines;
    std::size_t start = 0;
    double used = 0;
    for (std::size_t i = 0; i < items.size(); ++i) {
        spend(work_remaining);
        const auto& item = items[i];
        const double size = std::max(0.0, std::clamp(item.basis, item.minimum, item.maximum) + item.fixed);
        if (wrap == FlexWrap::Wrap && i > start && used + gap + size > width) {
            lines.push_back({start, i}); start = i; used = 0;
        }
        used += (i > start ? gap : 0) + size;
    }
    if (start < items.size()) lines.push_back({start, items.size()});
    for (const auto line : lines) resolve_line(items, line, width, gap, direction, justify, work_remaining);
    return lines;
}
} // namespace prowsetk::detail
