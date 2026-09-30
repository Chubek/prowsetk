#include "model.hpp"
#include <prowsetk/url.hpp>
#include <algorithm>
#include <cctype>
#include <stdexcept>
namespace prowsetk::tui {
std::string safe_text(std::string_view text) {
    std::string out;
    for (unsigned char c : text) {
        if (c == '\n' || c == '\t' || c == '\r') out += ' ';
        else if (c >= 32 && c != 127) out += static_cast<char>(c);
    }
    return out;
}
Page render(const ProwseEventStream& events, std::string_view base, std::size_t width) {
    width = std::clamp<std::size_t>(width, 10, 500);
    Page page;
    std::string line;
    std::size_t columns = 0;
    bool space = false;
    auto newline = [&] {
        if (!line.empty()) { page.lines.push_back(line); line.clear(); columns = 0; }
        space = false;
        if (page.lines.size() > 100000) throw std::runtime_error("Terminal layout limit exceeded");
    };
    auto append = [&](std::string_view text) {
        for (unsigned char c : safe_text(text)) {
            if (c == ' ') { space = !line.empty(); continue; }
            if ((c & 0xc0) != 0x80) {
                if (columns + (space ? 1 : 0) >= width) newline();
                if (space) { line += ' '; ++columns; space = false; }
                ++columns;
            }
            line += static_cast<char>(c);
        }
    };
    std::vector<bool> hidden;
    const auto block = [](std::string_view t) {
        return t == "p" || t == "div" || t == "section" || t == "article" || t == "li" ||
            t == "tr" || t == "h1" || t == "h2" || t == "h3" || t == "pre" || t == "blockquote";
    };
    for (const auto& e : events) {
        if (e.kind == "start") {
            auto attr = [&](std::string_view name) { for (const auto& a : e.attributes) if (a.name == name) return a.value; return std::string{}; };
            auto style = attr("style");
            style.erase(std::remove_if(style.begin(), style.end(), [](unsigned char c){return std::isspace(c);}), style.end());
            std::transform(style.begin(), style.end(), style.begin(), [](unsigned char c){return static_cast<char>(std::tolower(c));});
            bool hide = (!hidden.empty() && hidden.back()) || e.tag == "head" || e.tag == "script" || e.tag == "style" ||
                style.find("display:none") != std::string::npos || style.find("visibility:hidden") != std::string::npos;
            for (const auto& a : e.attributes) if (a.name == "hidden") hide = true;
            hidden.push_back(hide);
            if (hide) continue;
            if (block(e.tag) || e.tag == "br") newline();
            if (e.tag == "li") append("* ");
            if (e.tag == "td" || e.tag == "th") append(" | ");
            if (e.tag == "img") append("[image: " + attr("alt") + "]");
            if (e.tag == "input" || e.tag == "textarea" || e.tag == "select") append("[" + e.tag + "]");
            if (e.tag == "a" && !attr("href").empty()) {
                try {
                    auto url = resolve_url(base, attr("href"));
                    auto parsed = parse_url(url);
                    if (parsed.scheme == "http" || parsed.scheme == "https") {
                        page.links.push_back(url);
                        append("[" + std::to_string(page.links.size()) + "] ");
                    }
                } catch (const std::exception&) { /* Unsupported references are plain text. */ }
            }
        } else if (e.kind == "end") {
            if (!hidden.empty() && !hidden.back() && block(e.tag)) newline();
            if (!hidden.empty()) hidden.pop_back();
        } else if (e.kind == "text" && (hidden.empty() || !hidden.back())) append(e.value);
    }
    newline();
    return page;
}
}
