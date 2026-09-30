#include "help.hpp"
#include "model.hpp"
#include "help_pages.hpp"
#include <algorithm>
#include <iomanip>
#include <regex.h>
#include <sstream>
#include <stdexcept>

namespace prowsetk::tui {
namespace {
class Regex {
    regex_t value_{};
public:
    explicit Regex(std::string_view pattern) {
        if (pattern.empty() || pattern.size() > 1024 || pattern.find('\0') != std::string_view::npos)
            throw std::runtime_error("Regex must contain 1..1024 bytes");
        if (regcomp(&value_, std::string(pattern).c_str(), REG_EXTENDED | REG_NOSUB))
            throw std::runtime_error("Invalid help regex");
    }
    ~Regex() { regfree(&value_); }
    Regex(const Regex&) = delete;
    Regex& operator=(const Regex&) = delete;
    bool matches(const std::string& s) const { return regexec(&value_, s.c_str(), 0, nullptr, 0) == 0; }
};
std::string unescape(std::string_view raw) {
    std::string out;
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] != '\\' || i + 1 == raw.size()) { out += raw[i]; continue; }
        char c = raw[++i];
        if (c == 'f') { // Font escapes have no terminal side effects.
            if (i + 1 < raw.size() && raw[i + 1] == '[') {
                i = raw.find(']', i + 2); if (i == std::string_view::npos) break;
            } else if (i + 1 < raw.size()) ++i;
        } else if (c == '&' || c == ':') continue;
        else if (c == 'e' || c == '\\') out += '\\';
        else if (c == '-') out += '-';
        else if (c == '~' || c == ' ') out += ' ';
        else if (c == '(' && i + 2 < raw.size()) {
            auto glyph = raw.substr(i + 1, 2); i += 2;
            out += glyph == "bu" ? "*" : glyph == "em" ? "--" : glyph == "en" ? "-" : "?";
        } else out += c;
    }
    return safe_text(out);
}
std::vector<std::string> arguments(const std::string& text) {
    std::istringstream input(text);
    std::vector<std::string> args;
    std::string arg;
    while (input >> std::quoted(arg)) args.push_back(arg);
    return args;
}
std::string label(const std::string& text, bool join = false) {
    const auto args = arguments(text);
    std::string out;
    for (const auto& arg : args) { if (!out.empty() && !join) out += ' '; out += arg; }
    return unescape(out);
}
std::vector<std::size_t> matches(const std::vector<HelpLine>& lines, std::string_view pattern) {
    Regex regex(pattern);
    std::vector<std::size_t> result;
    for (std::size_t i = 0; i < lines.size(); ++i) if (regex.matches(lines[i].text)) result.push_back(i);
    return result;
}
std::vector<HelpLine> render_markup(std::string_view source, std::size_t width) {
    if (source.size() > 256 * 1024) throw std::runtime_error("Help page exceeds 256 KiB");
    width = std::clamp<std::size_t>(width, 10, 500); std::vector<HelpLine> out; std::string paragraph;
    auto flush = [&] { if (paragraph.empty()) return; std::istringstream words(paragraph); std::string word, line; while (words >> word) { if (!line.empty() && line.size()+1+word.size()>width) { out.push_back({line,{},0}); line.clear(); } if (!line.empty()) line += ' '; line += word; } if (!line.empty()) out.push_back({line,{},0}); paragraph.clear(); };
    std::istringstream input{std::string(source)}; std::string line;
    while (std::getline(input,line)) {
        if (line.empty()) { flush(); if (!out.empty() && !out.back().text.empty()) out.push_back({}); continue; }
        if (line.starts_with("#")) { flush(); auto n=line.find_first_not_of('#'); auto title=unescape(line.substr(n==std::string::npos?line.size():n)); out.push_back({title,"",0}); continue; }
        if (line.starts_with("@key ")) { flush(); out.push_back({unescape(line.substr(5)),"",0}); continue; }
        if (line.starts_with("@link ")) { flush(); auto split=line.find(' ' ,6); if(split!=std::string::npos){auto label=line.substr(6,split-6),target=line.substr(split+1);out.push_back({unescape(label),target,0});} continue; }
        if (line.starts_with("- ")) { flush(); auto open=line.find('['), close=line.find("](",open); if(open!=std::string::npos&&close!=std::string::npos&&line.back()==')'){out.push_back({unescape(line.substr(open+1,close-open-1)),line.substr(close+2,line.size()-close-3),0});} else paragraph += line.substr(2); continue; }
        if (!paragraph.empty()) paragraph += ' ';
        paragraph += unescape(line);
    }
    flush(); if (out.size()>20000) throw std::runtime_error("Help layout exceeds line limit"); return out;
}
}
std::map<std::string, std::string> bundled_help() {
    std::map<std::string, std::string> result;
    for (const auto& entry : help_sources) result.emplace(entry.name, entry.source);
    return result;
}
std::vector<HelpLine> render_man(std::string_view source, std::size_t width) {
    if (source.size() > 256 * 1024) throw std::runtime_error("Help page exceeds 256 KiB");
    width = std::clamp<std::size_t>(width, 10, 500);
    std::vector<HelpLine> out;
    std::string paragraph, target;
    bool literal = false;
    auto flush = [&] {
        if (paragraph.empty()) return;
        std::istringstream words(paragraph);
        std::string word, line;
        while (words >> word) {
            if (!line.empty() && line.size() + 1 + word.size() > width) { out.push_back({line, target, 0}); line.clear(); }
            if (!line.empty()) line += ' ';
            line += word;
        }
        if (!line.empty()) out.push_back({line, target, 0});
        paragraph.clear();
    };
    auto blank = [&] { flush(); if (!out.empty() && !out.back().text.empty()) out.push_back({}); };
    std::istringstream input{std::string(source)};
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty()) { blank(); continue; }
        if (line[0] != '.' && line[0] != '\'') {
            if (literal) { flush(); out.push_back({unescape(line), target, 0}); }
            else { if (!paragraph.empty()) paragraph += ' '; paragraph += unescape(line); }
            continue;
        }
        std::istringstream macro(line.substr(1));
        std::string name, rest; macro >> name; std::getline(macro >> std::ws, rest);
        if (name == "TH" || name == "SH" || name == "SS") { blank(); paragraph = label(rest); flush(); }
        else if (name == "PP" || name == "P" || name == "LP" || name == "TP" || name == "TQ" || name == "sp") blank();
        else if (name == "br") flush();
        else if (name == "IP") { blank(); auto args = arguments(rest); if (!args.empty()) paragraph = unescape(args.front()); }
        else if (name == "B" || name == "I" || name == "SM" || name == "SB" || name == "BR" || name == "BI" || name == "IR" || name == "IB" || name == "RB" || name == "RI") {
            if (!paragraph.empty()) paragraph += ' ';
            paragraph += label(rest, name.size() == 2 && name != "SM" && name != "SB");
        } else if (name == "nf" || name == "EX") { flush(); literal = true; }
        else if (name == "fi" || name == "EE") { flush(); literal = false; }
        else if (name == "UR") { flush(); target = label(rest); }
        else if (name == "UE") { if (paragraph.empty()) paragraph = target; flush(); target.clear(); if (!rest.empty()) paragraph = label(rest); }
        else if (name == "MR") {
            flush(); auto args = arguments(rest);
            if (!args.empty()) {
                out.push_back({args[0] + (args.size() > 1 ? "(" + args[1] + ")" : ""), "help:" + args[0], 0});
            }
        }
        // Unknown requests, includes, definitions and shell commands are never executed.
    }
    flush();
    if (out.size() > 20000) throw std::runtime_error("Help layout exceeds line limit");
    return out;
}
std::vector<HelpLine> render_help(std::string_view source, std::size_t width) {
    if (source.find("@key ") != std::string_view::npos || source.find("@link ") != std::string_view::npos || source.starts_with("#")) return render_markup(source, width);
    return render_man(source, width);
}
HelpPager::HelpPager(std::map<std::string, std::string> sources) : sources_(std::move(sources)) {
    if (sources_.size() > 128) throw std::runtime_error("Too many help pages");
    for (const auto& [name, source] : sources_) {
        if (name.empty() || source.size() > 256 * 1024) throw std::runtime_error("Invalid help collection");
    }
}
void HelpPager::show(View next) {
    layout(next); // Invalid search/page leaves the current view and history intact.
    if (!current_.title.empty()) {
        if (history_.size() == 64) history_.erase(history_.begin());
        history_.push_back(std::move(current_));
    }
    current_ = std::move(next);
}
void HelpPager::layout(View& view) const {
    view.lines.clear();
    if (!view.page.empty()) view.lines = render_help(sources_.at(view.page), width_);
    else if (!view.find_pattern.empty()) {
        Regex regex(view.find_pattern);
        for (const auto& [name, source] : sources_) {
            auto lines = render_help(source, width_);
            for (std::size_t i = 0; i < lines.size(); ++i) {
                if (regex.matches(lines[i].text)) view.lines.push_back({name + ": " + lines[i].text, "help:" + name, i});
                if (view.lines.size() >= 20000) throw std::runtime_error("Too many help results; narrow the regex");
            }
        }
        if (view.lines.empty()) view.lines.push_back({"No matching help pages", {}, 0});
    } else for (const auto& [name, source] : sources_) {
        (void)source;
        view.lines.push_back({name, "help:" + name, 0});
    }
    view.cursor = std::min(view.cursor, view.lines.empty() ? 0 : view.lines.size() - 1);
    view.matches = view.search_pattern.empty() ? std::vector<std::size_t>{} : matches(view.lines, view.search_pattern);
}
void HelpPager::open(std::string_view name, std::size_t line) {
    std::string page(name);
    if (page.starts_with("help:")) page.erase(0, 5);
    if (!sources_.contains(page)) throw std::runtime_error("Unknown help page; use :help list");
    View next; next.title = "Help: " + page; next.page = page; next.cursor = line; show(std::move(next));
}
void HelpPager::index() { View next; next.title = "Help index"; show(std::move(next)); }
void HelpPager::find(std::string_view pattern) {
    View next; next.title = "Help search results"; next.find_pattern = std::string(pattern);
    if (pattern.empty()) throw std::runtime_error("Help find requires a regex");
    show(std::move(next));
}
std::size_t HelpPager::search(std::string_view pattern) {
    auto found = matches(current_.lines, pattern);
    current_.search_pattern = pattern; current_.matches = std::move(found);
    if (!current_.matches.empty()) current_.cursor = current_.matches.front();
    return current_.matches.size();
}
bool HelpPager::next_match(bool backwards) {
    const auto& found = current_.matches;
    if (found.empty()) return false;
    if (backwards) {
        auto it = std::lower_bound(found.begin(), found.end(), current_.cursor);
        current_.cursor = it == found.begin() ? found.back() : *std::prev(it);
    } else {
        auto it = std::upper_bound(found.begin(), found.end(), current_.cursor);
        current_.cursor = it == found.end() ? found.front() : *it;
    }
    return true;
}
void HelpPager::resize(std::size_t width) {
    width = std::clamp<std::size_t>(width, 10, 500);
    if (width != width_) { width_ = width; if (!current_.title.empty()) layout(current_); }
}
void HelpPager::move(int delta) {
    if (delta < 0) current_.cursor -= std::min(current_.cursor, static_cast<std::size_t>(-static_cast<long long>(delta)));
    else current_.cursor = std::min(current_.cursor + static_cast<std::size_t>(delta), current_.lines.empty() ? 0 : current_.lines.size() - 1);
}
void HelpPager::back() {
    if (!history_.empty()) { current_ = std::move(history_.back()); history_.pop_back(); layout(current_); }
}
std::string HelpPager::activate() {
    if (current_.cursor >= current_.lines.size()) return {};
    const auto link = current_.lines[current_.cursor];
    if (link.target.starts_with("help:")) open(link.target, link.target_line);
    else if (link.target.starts_with("https://") || link.target.starts_with("http://")) return link.target;
    else if (!link.target.empty()) throw std::runtime_error("Unsupported help link");
    return {};
}
bool HelpPager::matched(std::size_t line) const { return std::binary_search(current_.matches.begin(), current_.matches.end(), line); }
void HelpPager::home() { current_.cursor = 0; }
void HelpPager::end() { current_.cursor = current_.lines.empty() ? 0 : current_.lines.size() - 1; }
}
