// DOM reduction and prompt preparation for the OpenCode bridge.
//
// Operates on detached parses only; live session Documents are never mutated.
// Heavy multimedia, inline base64 assets, and excessive SVG definitions are
// stripped before a snapshot is packaged into a prompt. Semantic tags and
// data attributes are retained.

#include "opencode_bridge.hpp"

#include <algorithm>
#include <array>
#include <cctype>

#include "prowsetk/error.hpp"

namespace prowsetk::plugins::opencode_bridge {
namespace {

bool is_data_attr(std::string_view name) {
    return name.size() > 5 && name.substr(0, 5) == "data-" && name.find(' ') == std::string_view::npos;
}

bool keep_attribute(std::string_view name) {
    static constexpr std::array<std::string_view, 13> kKept = {
        "id", "class", "role", "type", "name", "href", "action",
        "title", "alt", "aria-label", "aria-disabled", "disabled", "checked"};
    if (is_data_attr(name)) return true;
    return std::find(kKept.begin(), kKept.end(), name) != kKept.end();
}

bool heavy_tag(std::string_view tag) {
    static constexpr std::array<std::string_view, 15> kHeavy = {
        "script", "style", "img", "video", "audio", "source", "track",
        "canvas", "iframe", "embed", "object", "noscript", "picture",
        "template", "slot"};
    return std::find(kHeavy.begin(), kHeavy.end(), tag) != kHeavy.end();
}

// Replace the payload of data: URLs carrying base64 with a placeholder,
// without truncating the surrounding text.
std::string strip_base64_payloads(std::string value) {
    std::string::size_type at = 0;
    for (;;) {
        const auto marker = value.find("base64,", at);
        if (marker == std::string::npos) break;
        auto end = marker + 7;
        while (end < value.size() && value[end] != '"' && value[end] != '\'' &&
               value[end] != ' ' && value[end] != ')' && end - marker < 4096 + 7) {
            ++end;
        }
        value.replace(marker + 7, end - (marker + 7), "[STRIPPED]");
        at = marker + 7 + 10;
    }
    return value;
}

std::string truncate_base64(std::string value) {
    value = strip_base64_payloads(std::move(value));
    if (value.size() > 512) {
        value.resize(512);
        value += "[TRUNCATED]";
    }
    return value;
}

std::string clipped(std::string text, std::size_t budget) {
    if (text.size() <= budget) return text;
    text.resize(budget);
    text += "[TRUNCATED]";
    return text;
}

}  // namespace

PageSnapshot PageSnapshot::from_document(const Document& document) {
    PageSnapshot snapshot;
    if (!document.valid()) return snapshot;
    snapshot.url = document.url();
    snapshot.title = document.title();
    snapshot.html = document.html();
    snapshot.text = document.text();
    return snapshot;
}

std::string summarize_snapshot(const PageSnapshot& snapshot, std::size_t budget) {
    std::string summary = "url: " + snapshot.url + "\ntitle: " + snapshot.title + "\ntext:\n";
    const std::size_t headroom = summary.size() + 16;
    const std::size_t text_budget = headroom >= budget ? 0 : budget - headroom;
    summary += clipped(snapshot.text, text_budget);
    return summary;
}

std::string sanitize_html(std::string_view html, std::size_t budget) {
    if (html.empty()) return {};
    if (budget == 0) throw Error(ErrorCode::InvalidArgument, "opencode-bridge: invalid sanitize budget");
    std::shared_ptr<Document> detached;
    try {
        detached = parse_html(html);
    } catch (const Error&) {
        throw Error(ErrorCode::ParseError, "opencode-bridge: unable to parse HTML snapshot");
    }
    if (!detached || !detached->valid()) return {};

    auto elements = detached->query_selector_all("*");
    std::size_t svg_count = 0;
    for (const auto& element : elements) {
        if (element && element->tag_name() == "svg") ++svg_count;
    }
    const bool drop_svg = svg_count > 8;

    for (const auto& element : elements) {
        if (!element || !element->valid()) continue;
        const std::string tag = element->tag_name();
        if (heavy_tag(tag) || (drop_svg && (tag == "svg" || tag == "path" || tag == "defs"))) {
            element->set_text("");
        }
        // Collect first: mutation while iterating the attribute vector is safe
        // because attributes() returns a copy.
        for (const auto& attr : element->attributes()) {
            if (!keep_attribute(attr.name)) {
                element->remove_attribute(attr.name);
            } else {
                element->set_attribute(attr.name, truncate_base64(attr.value));
            }
        }
    }

    std::string sanitized = strip_base64_payloads(detached->html());
    if (sanitized.size() > budget) {
        PageSnapshot fallback;
        fallback.title = detached->title();
        fallback.text = detached->text();
        return summarize_snapshot(fallback, budget);
    }
    return sanitized;
}

std::string build_endpoint_cleanup_prompt(std::string_view endpoints_json,
                                          std::string_view instructions,
                                          std::size_t max_input_bytes) {
    if (endpoints_json.empty()) {
        throw Error(ErrorCode::InvalidArgument, "opencode-bridge: endpoint list is required");
    }
    if (max_input_bytes == 0 || max_input_bytes > kMaxInputBytesHardCap) {
        throw Error(ErrorCode::InvalidArgument, "opencode-bridge: invalid input budget");
    }
    std::string prompt =
        "You are cleaning a heuristically scraped web-API endpoint list. "
        "The input is a JSON array of endpoint objects with url, method, path, "
        "discovery_method and confidence fields. "
        "RULES: (1) Return a JSON array containing ONLY a subset of the input "
        "objects, copied verbatim (same url and method values). "
        "(2) NEVER invent, normalize, or rewrite urls or methods. "
        "(3) DROP entries that cannot serve as API endpoints: static assets "
        "(images, stylesheets, fonts, bundles), analytics/beacon pings, "
        "challenge/telemetry URLs, plain navigation pages, and duplicates. "
        "(4) When in doubt, KEEP the entry. "
        "Return the JSON array only, no prose.\n";
    if (!instructions.empty()) {
        prompt += "Additional instructions: " + std::string(instructions) + "\n";
    }
    prompt += "endpoints:\n" + std::string(endpoints_json);
    if (prompt.size() > max_input_bytes) {
        throw Error(ErrorCode::ResourceLimit, "opencode-bridge: input byte limit exceeded");
    }
    return prompt;
}

std::string build_scrape_prompt(const PageSnapshot& snapshot, std::string_view instructions,
                                std::string_view schema) {
    if (instructions.empty()) throw Error(ErrorCode::InvalidArgument, "opencode-bridge: instructions are required");
    std::string page = snapshot.html.size() > kSanitizedHtmlBudget
        ? summarize_snapshot(snapshot)
        : sanitize_html(snapshot.html);
    if (page.empty()) page = summarize_snapshot(snapshot);
    std::string prompt = "Extract structured data from the following page.\n";
    prompt += "url: " + snapshot.url + "\ntitle: " + snapshot.title + "\n";
    prompt += "instructions: " + std::string(instructions) + "\n";
    if (!schema.empty()) prompt += "schema (JSON object): " + std::string(schema) + "\n";
    prompt += "Return a JSON object only.\npage:\n" + page;
    return prompt;
}

}  // namespace prowsetk::plugins::opencode_bridge
