#include <prowsetk/plugins/basic_gui.hpp>

#include <algorithm>
#include <cctype>
#include <string_view>

#include <prowsetk/ir.hpp>
#include <prowsetk/redaction.hpp>

namespace prowsetk::basic_gui {
namespace {

std::string escape(std::string_view text) {
    std::string result;
    for (unsigned char c : text) {
        switch (c) {
            case '&': result += "&amp;"; break;
            case '<': result += "&lt;"; break;
            case '>': result += "&gt;"; break;
            case '"': result += "&quot;"; break;
            default: if (c >= 32 || c == '\n' || c == '\t') result += static_cast<char>(c);
        }
    }
    return result;
}

bool in(std::string_view word, std::string_view list) {
    return list.find(" " + std::string(word) + " ") != std::string_view::npos;
}

bool safe_name(std::string_view name) {
    return !name.empty() && std::all_of(name.begin(), name.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '-' || c == '_' || c == ':';
    });
}

std::string attribute(const std::vector<Attribute>& attrs, std::string_view name) {
    for (const auto& attr : attrs) if (attr.name == name) return attr.value;
    return {};
}

bool has(const std::vector<Attribute>& attrs, std::string_view name) {
    return std::any_of(attrs.begin(), attrs.end(), [name](const Attribute& a) { return a.name == name; });
}

bool sensitive(std::string name) {
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const Redactor redactor;
    if (redactor.is_sensitive_header(name) || redactor.is_sensitive_query_parameter(name)) return true;
    for (auto part : {"token", "secret", "password", "credential", "cookie", "csrf", "api-key", "api_key"}) {
        if (name.find(part) != std::string::npos) return true;
    }
    return false;
}

bool inline_hidden(std::string style) {
    std::transform(style.begin(), style.end(), style.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    style.erase(std::remove_if(style.begin(), style.end(), [](unsigned char c) { return std::isspace(c); }), style.end());
    return style.find("display:none") != std::string::npos || style.find("visibility:hidden") != std::string::npos;
}

Snapshot limited(std::uint64_t revision) {
    Snapshot result;
    result.revision = revision;
    result.limited = true;
    result.preview_html = "<p>Page loaded; inspection snapshot exceeds the size, node or depth bounds.</p>";
    result.source_html = "[inspection snapshot limited]";
    return result;
}

}  // namespace

Snapshot inspect_document(const Document& document, std::uint64_t revision) {
    // Preflight before emitting IR: subtree_text duplicates ancestor text, so
    // both size and depth need bounds. Later DOM mutations have no parser quota.
    std::vector<std::pair<std::shared_ptr<Element>, std::size_t>> pending;
    if (auto root = document.root()) pending.emplace_back(std::move(root), 0);
    std::size_t count = 0;
    while (!pending.empty()) {
        auto [element, depth] = std::move(pending.back());
        pending.pop_back();
        if (++count > max_snapshot_nodes || depth > max_snapshot_depth) return limited(revision);
        for (auto& child : element->children()) pending.emplace_back(std::move(child), depth + 1);
    }
    if (document.html().size() > max_snapshot_bytes) return limited(revision);

    Snapshot result;
    result.revision = revision;
    result.url = Redactor{}.redact_url(document.url());
    result.title = document.title().substr(0, 256);
    result.preview_html = "<html><body>";
    struct Frame { std::string source_tag, preview_tag; bool suppress_text, visible, collapsed, disabled; };
    std::vector<Frame> stack;
    const Redactor redactor;
    for (const auto& event : emit_prowse_events(document)) {
        if (event.kind == "start") {
            const auto& tag = event.tag;
            auto attrs = event.attributes;
            const bool private_text = in(tag, " script style textarea option template ");
            const bool inherited_private = !stack.empty() && stack.back().suppress_text;
            const bool hidden = (!stack.empty() && !stack.back().visible) || has(attrs, "hidden") ||
                (!stack.empty() && stack.back().collapsed && tag != "summary") ||
                inline_hidden(attribute(attrs, "style")) || in(tag, " head script style template meta link title ") ||
                (tag == "dialog" && !has(attrs, "open")) || (tag == "input" && attribute(attrs, "type") == "hidden");
            const bool disabled = (!stack.empty() && stack.back().disabled) || has(attrs, "disabled") || attribute(attrs, "aria-disabled") == "true";
            for (auto& attr : attrs) {
                if (sensitive(attr.name) || attr.name.starts_with("on") || attr.name == "srcdoc" ||
                    attr.name == "style" || attr.name == "srcset" || (tag == "meta" && attr.name == "content") ||
                    (in(tag, " input textarea select option ") && attr.name == "value")) {
                    attr.value = "[REDACTED]";
                } else if (in(attr.name, " href src action formaction poster ")) {
                    const auto colon = attr.value.find(':');
                    const auto slash = attr.value.find('/');
                    const bool scheme = colon != std::string::npos && (slash == std::string::npos || colon < slash);
                    if (scheme && !attr.value.starts_with("http:") && !attr.value.starts_with("https:")) attr.value = "[URL omitted]";
                    else attr.value = redactor.redact_url(attr.value);
                }
                if (attr.value.size() > 1024) attr.value = "[value limited]";
            }
            const auto index = result.nodes.size();
            result.nodes.push_back({event.xpath, tag, attrs, event.depth, hidden, disabled});
            const std::string source_tag = safe_name(tag) ? tag : "unknown";
            result.source_html += "<" + source_tag;
            for (const auto& attr : attrs) if (safe_name(attr.name)) {
                result.source_html += " " + attr.name + "=\"" + escape(attr.value) + "\"";
            }
            result.source_html += ">";
            std::string preview_tag;
            if (!hidden) {
                if (in(tag, " a button summary input textarea select ")) {
                    preview_tag = "a";
                    result.preview_html += "<a href=\"prowse-action:" + std::to_string(revision) + ":" + std::to_string(index) + "\">";
                    if (in(tag, " input textarea select ")) result.preview_html += "[" + tag + ": value hidden]";
                } else if (tag == "img") {
                    result.preview_html += "[image: " + escape(attribute(attrs, "alt")) + "]";
                } else if (in(tag, " h1 h2 h3 h4 h5 h6 p pre b strong i em u code tt ul ol li table tr td th br hr ")) {
                    preview_tag = tag == "code" ? "tt" : tag;
                    result.preview_html += "<" + preview_tag + ">";
                } else if (in(tag, " div section article main form details dialog header footer ")) {
                    preview_tag = "p";
                    result.preview_html += "<p>";
                }
            }
            stack.push_back({source_tag, preview_tag, private_text || inherited_private, !hidden,
                             tag == "details" && !has(attrs, "open"), disabled});
            if (private_text && !inherited_private) result.source_html += "[content omitted]";
        } else if (event.kind == "text" && !stack.empty() && !stack.back().suppress_text) {
            const auto text = escape(event.value);
            result.source_html += text;
            if (stack.back().visible) result.preview_html += text;
        } else if (event.kind == "end" && !stack.empty()) {
            const auto frame = std::move(stack.back());
            stack.pop_back();
            if (!in(frame.source_tag, " area base br col embed hr img input link meta param source track wbr ")) {
                result.source_html += "</" + frame.source_tag + ">";
            }
            if (!frame.preview_tag.empty() && !in(frame.preview_tag, " br hr ")) {
                result.preview_html += "</" + frame.preview_tag + ">";
            }
        }
        if (result.source_html.size() > max_snapshot_bytes || result.preview_html.size() > max_snapshot_bytes) return limited(revision);
    }
    result.preview_html += "</body></html>";
    return result;
}

}  // namespace prowsetk::basic_gui
