#include "model.hpp"
#include <termbox2.h>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <clocale>
namespace {
class Terminal {
public:
    Terminal() { if (tb_init() < 0) throw std::runtime_error("Cannot initialize terminal"); }
    ~Terminal() { tb_shutdown(); }
};
std::string read_html(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Cannot read HTML file");
    std::string data;
    char chunk[8192];
    while (file.read(chunk, sizeof(chunk)) || file.gcount()) {
        data.append(chunk, static_cast<std::size_t>(file.gcount()));
        if (data.size() > 16 * 1024 * 1024) throw std::runtime_error("HTML exceeds 16 MiB");
    }
    return data;
}
void paint(int y, const std::string& text, uintattr_t color = TB_DEFAULT) {
    const auto safe = prowsetk::tui::safe_text(text);
    int x = 0;
    for (std::size_t i = 0; i < safe.size() && x < tb_width();) {
        uint32_t ch = 0;
        int bytes = tb_utf8_char_to_unicode(&ch, safe.c_str() + i);
        if (bytes <= 0 || i + static_cast<std::size_t>(bytes) > safe.size()) { ch = '?'; bytes = 1; }
        if ((ch >= 0x80 && ch <= 0x9f) || ch == 0x202e || ch == 0x202d) ch = '?';
        tb_set_cell(x, y, ch, color, TB_DEFAULT);
        x += std::max(1, tb_wcwidth(ch));
        i += static_cast<std::size_t>(bytes);
    }
}
}
int main(int argc, char** argv) {
    try {
        std::setlocale(LC_ALL, "");
        prowsetk::tui::Controller app;
        try { app.initialize_settings(prowsetk::tui::default_settings_path()); }
        catch (const std::exception&) { /* Missing or invalid optional config does not prevent startup. */ }
        bool dump = false;
        std::vector<std::string> commands;
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--help") { std::cout << "prowse-tui [URL] [--html FILE] [--dump] [--command COMMAND]\n" << app.command("help") << '\n'; return 0; }
            if (arg == "--dump") dump = true;
            else if (arg == "--html" && i + 1 < argc) app.session->load_html(read_html(argv[++i]), "https://localhost/");
            else if (arg == "--command" && i + 1 < argc) commands.emplace_back(argv[++i]);
            else if (arg.starts_with("--")) throw std::runtime_error("Unknown option or missing value");
            else app.command("open " + arg);
        }
        app.refresh();
        for (const auto& command : commands) std::cout << app.command(command) << '\n';
        if (dump) { for (const auto& line : app.page.lines) std::cout << line << '\n'; return 0; }
        Terminal terminal;
        std::string command, status = ":help for commands; j/k scroll; q quit", panel;
        bool editing = false, searching = false;
        for (;;) {
            const int height = tb_height();
            const int width = tb_width();
            app.refresh(static_cast<std::size_t>(std::max(10, width)));
            tb_clear();
            const auto visible = app.visible_lines();
            const auto cursor = app.view == prowsetk::tui::ViewMode::Help ? app.help.cursor() : app.view == prowsetk::tui::ViewMode::Config ? app.config_cursor : app.top;
            const auto base = app.view == prowsetk::tui::ViewMode::Browser ? app.top : (cursor > static_cast<std::size_t>(std::max(1, height - 3)) ? cursor - static_cast<std::size_t>(std::max(1, height - 3)) : 0);
            if (height > 0) paint(0, app.view == prowsetk::tui::ViewMode::Browser ? "Prowse-TUI | " + app.session->document()->title() : app.view == prowsetk::tui::ViewMode::Help ? "Help | " + app.help.title() : "Prowse-TUI Config", TB_CYAN);
            for (int row = 1; row < height - 2; ++row) {
                auto index = base + static_cast<std::size_t>(row - 1);
                if (index < visible.size()) paint(row, (index == cursor && app.view != prowsetk::tui::ViewMode::Browser ? "> " : "") + visible[index]);
            }
            if (!panel.empty()) {
                std::istringstream lines(panel);
                std::string line;
                int row = 1;
                while (row < height - 2 && std::getline(lines, line)) paint(row++, line, TB_YELLOW);
            }
            if (height >= 2) paint(height - 2, status, TB_GREEN);
            if (height >= 1) paint(height - 1, editing ? (searching ? "/" : ":") + command : "", TB_YELLOW);
            tb_present();
            tb_event ev{};
            if (tb_poll_event(&ev) < 0) throw std::runtime_error("Terminal input failed");
            if (ev.type != TB_EVENT_KEY) continue;
            if (ev.key == TB_KEY_CTRL_C) break;
            if (editing) {
                if (ev.key == TB_KEY_ESC) { editing = false; searching = false; command.clear(); }
                else if (ev.key == TB_KEY_ENTER) {
                    // Lua may print. Leave raw mode while running user automation.
                    tb_shutdown();
                    try {
                        if (searching) { const auto count = app.help.search(command); panel = std::to_string(count) + " matches"; status = "n next, ? previous, Esc closes search"; }
                        else { auto action = app.command(command); panel = action; if (app.view != prowsetk::tui::ViewMode::Browser && action.rfind("config ", 0) == 0) { auto nested = app.command(action); panel = nested; } status = "Command completed; Esc closes result"; }
                    } catch (const std::exception& error) { panel.clear(); status = error.what(); }
                    if (tb_init() < 0) throw std::runtime_error("Cannot restore terminal");
                    command.clear(); editing = false; searching = false;
                } else if (ev.key == TB_KEY_BACKSPACE || ev.key == TB_KEY_BACKSPACE2) {
                    if (!command.empty()) { do { auto c = static_cast<unsigned char>(command.back()); command.pop_back(); if ((c & 0xc0) != 0x80) break; } while (!command.empty()); }
                } else if (ev.ch && command.size() < 65532) {
                    char utf8[5]{}; const int n = tb_utf8_unicode_to_char(utf8, ev.ch);
                    if (n > 0) command.append(utf8, static_cast<std::size_t>(n));
                }
            } else if (ev.ch == ':') { editing = true; searching = false; panel.clear(); }
            else if (ev.ch == '/' && app.view == prowsetk::tui::ViewMode::Help) { editing = true; searching = true; command.clear(); }
            else if (ev.key == TB_KEY_ESC) { if (app.view == prowsetk::tui::ViewMode::Browser) panel.clear(); else app.close_view(); }
            else if (ev.ch == 'q' && app.view == prowsetk::tui::ViewMode::Browser) break;
            else {
                std::string action;
                if (ev.ch == 'j' || ev.key == TB_KEY_ARROW_DOWN) action = "down";
                else if (ev.ch == 'k' || ev.key == TB_KEY_ARROW_UP) action = "up";
                else if (ev.ch == 'h' || ev.key == TB_KEY_ARROW_LEFT) action = "back";
                else if (ev.ch == 'l' || ev.key == TB_KEY_ARROW_RIGHT || ev.key == TB_KEY_ENTER) action = "activate";
                else if (ev.ch == 'n') action = "next_match";
                else if (ev.ch == '?') action = "previous_match";
                else if (ev.key == TB_KEY_PGDN || ev.ch == ' ') action = "page_down";
                else if (ev.key == TB_KEY_PGUP || ev.ch == 'b') action = "page_up";
                else if (ev.ch == 'g') action = "home";
                else if (ev.ch == 'G') action = "end";
                else if (ev.ch == 'q') action = "close";
                else if (ev.ch == TB_KEY_F1) action = "help";
                else if (ev.ch == TB_KEY_F2) action = "config";
                try { if (!action.empty()) { auto nested = app.key_action(action, height - 3); if (!nested.empty()) panel = app.command(nested); } }
                catch (const std::exception& error) { status = error.what(); }
            }
            if (app.quit) break;
        }
        return 0;
    } catch (const std::exception&) { std::cerr << "prowse-tui: operation failed\n"; return 1; }
}
