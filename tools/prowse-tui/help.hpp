#pragma once
#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace prowsetk::tui {
struct HelpLine {
    std::string text;
    std::string target;
    std::size_t target_line = 0;
};
// A bounded, non-executing subset of the Unix man macro language.
std::vector<HelpLine> render_man(std::string_view source, std::size_t width);
std::map<std::string, std::string> bundled_help();
class HelpPager {
public:
    explicit HelpPager(std::map<std::string, std::string> sources = bundled_help());
    void open(std::string_view name, std::size_t line = 0);
    void index();
    void find(std::string_view regex);
    std::size_t search(std::string_view regex);
    bool next_match(bool backwards = false);
    void resize(std::size_t width);
    void move(int delta);
    void back();
    // Local links open in the pager; HTTP(S) targets are returned to the host.
    std::string activate();
    const std::vector<HelpLine>& lines() const { return current_.lines; }
    const std::string& title() const { return current_.title; }
    std::size_t cursor() const { return current_.cursor; }
    bool matched(std::size_t line) const;
    void home();
    void end();
private:
    struct View {
        std::string title;
        std::string page;
        std::string find_pattern;
        std::vector<HelpLine> lines;
        std::size_t cursor = 0;
        std::string search_pattern;
        std::vector<std::size_t> matches;
    } current_;
    std::map<std::string, std::string> sources_;
    std::vector<View> history_;
    std::size_t width_ = 80;
    void show(View next);
    void layout(View& view) const;
};
}
