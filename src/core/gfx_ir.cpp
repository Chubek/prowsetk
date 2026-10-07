#include "prowsetk/gfx_ir.hpp"
#include "core/event_stream.hpp"
#include "prowsetk/error.hpp"

#include <algorithm>
#include <cctype>

namespace prowsetk {
namespace {
bool block(std::string_view tag) {
    return tag == "p" || tag == "div" || tag == "section" || tag == "article" ||
        tag == "main" || tag == "li" || tag == "tr" || tag == "pre" ||
        tag == "blockquote" || (tag.size() == 2 && tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '6');
}
bool hidden(const ProwseEvent& event) {
    if (event.tag == "head" || event.tag == "script" || event.tag == "style" ||
        event.tag == "template" || event.tag == "textarea" || event.tag == "select") return true;
    for (const auto& attribute : event.attributes) {
        if (attribute.name == "hidden") return true;
        if (attribute.name != "style") continue;
        auto style = attribute.value;
        style.erase(std::remove_if(style.begin(), style.end(),
            [](unsigned char c) { return std::isspace(c); }), style.end());
        std::transform(style.begin(), style.end(), style.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (style.find("display:none") != std::string::npos ||
            style.find("visibility:hidden") != std::string::npos) return true;
    }
    return false;
}
}

GfxBytecode emit_gfx_ir(std::span<const ProwseEvent> events, GfxOptions options) {
    // Bound all caller-supplied event values before lowering.
    if (events.size() > max_event_stream_events)
        throw Error(ErrorCode::ResourceLimit, "GFX source event limit exceeded");
    EventStreamBudget budget;
    for (const auto& event : events) budget.consume(event);
    const EventStreamReader reader(ProwseEventStream(events.begin(), events.end()));
    GfxFrame frame{options, {}};
    (void)encode_gfx_frame(frame); // Validate dimensions even for empty input.
    struct Scope { std::string tag; std::string path; bool hide; };
    std::vector<Scope> stack;
    std::string line;
    std::size_t columns = 0;
    bool space = false;
    std::size_t bytes = 17;
    const auto width = std::max<std::uint32_t>(1, (options.width > 32 ? options.width - 32 : 8) / 8);
    const auto newline = [&]() {
        if (!line.empty()) {
            if (frame.text.size() >= max_gfx_commands ||
                16 + frame.text.size() * 18 > max_gfx_content_height ||
                13 + line.size() > max_gfx_bytes - bytes)
                throw Error(ErrorCode::ResourceLimit, "GFX preview limit exceeded");
            bytes += 13 + line.size();
            frame.text.push_back({std::min<std::uint32_t>(16, options.width - 1),
                static_cast<std::uint32_t>(16 + frame.text.size() * 18), std::move(line)});
            line.clear(); columns = 0;
        }
        space = false;
    };
    const auto append = [&](std::string_view text) {
        for (std::size_t i = 0; i < text.size();) {
            const auto c = static_cast<unsigned char>(text[i]);
            if (c <= 32 || c == 127) { space = !line.empty(); ++i; continue; }
            std::size_t n = c < 128 ? 1u : c >= 0xc2 && c <= 0xdf ? 2u :
                c >= 0xe0 && c <= 0xef ? 3u : c >= 0xf0 && c <= 0xf4 ? 4u : 0u;
            if (!n || n > text.size() - i)
                throw Error(ErrorCode::ParseError, "invalid GFX preview text");
            if (columns + (space ? 1u : 0u) >= width || line.size() + n + 1 > max_gfx_text_bytes) newline();
            if (space) { line += ' '; ++columns; space = false; }
            line.append(text.substr(i, n)); ++columns; i += n;
        }
    };
    reader.replay([&](const ProwseEvent& event) {
        const auto invalid = [] { throw Error(ErrorCode::ParseError, "invalid GFX event stream"); };
        if (event.kind == "start") {
            if (stack.size() >= 256 || event.depth != stack.size() || event.tag.empty()) invalid();
            const bool hide = (!stack.empty() && stack.back().hide) || hidden(event);
            stack.push_back({event.tag, event.xpath, hide});
            if (!hide) {
                if (block(event.tag) || event.tag == "br") newline();
                if (event.tag == "li") append("* ");
                if (event.tag == "input") append("[input]");
                if (event.tag == "img") append("[image]");
            }
        } else if (event.kind == "end") {
            if (stack.empty() || event.depth != stack.size() - 1 ||
                event.tag != stack.back().tag || event.xpath != stack.back().path) invalid();
            if (!stack.back().hide && block(event.tag)) newline();
            stack.pop_back();
        } else if (event.kind == "text") {
            if (stack.empty() || event.depth != stack.size() || event.tag != stack.back().tag) invalid();
            if (!stack.back().hide) append(event.value);
        } else if (event.kind == "attribute") {
            if (stack.empty() || event.depth != stack.size() - 1 ||
                event.tag != stack.back().tag || event.xpath != stack.back().path) invalid();
        } else invalid();
        return EventStreamAction::Continue;
    });
    if (!stack.empty()) throw Error(ErrorCode::ParseError, "incomplete GFX event stream");
    newline();
    return encode_gfx_frame(frame);
}
}  // namespace prowsetk
