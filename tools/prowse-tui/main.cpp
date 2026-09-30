#include "model.hpp"
#include "terminal.h"
#include <algorithm>
#include <clocale>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
std::string read_html(const std::string& path) {
    std::ifstream file(path, std::ios::binary); if (!file) throw std::runtime_error("Cannot read HTML file");
    std::string data((std::istreambuf_iterator<char>(file)), {}); if (data.size() > 16 * 1024 * 1024) throw std::runtime_error("HTML exceeds 16 MiB"); return data;
}
void paint(int row, const std::string& text, int color, bool inverse = false) { const auto safe = prowsetk::tui::safe_text(text); pwtk_terminal_line(row, safe.c_str(), color, inverse); }
}

int main(int argc, char** argv) {
    try {
        std::setlocale(LC_ALL, ""); prowsetk::tui::Controller app;
        (void)pwtk_termscript_load_ui();
        try { app.initialize_settings(prowsetk::tui::default_settings_path()); } catch (...) {}
        bool dump = false; std::vector<std::string> commands;
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--help") { app.command("help"); std::cout << "prowse-tui [URL] [--html FILE] [--dump] [--command COMMAND]\n\n"; for (const auto& line : app.help.lines()) std::cout << line.text << '\n'; return 0; }
            if (arg == "--dump") dump = true;
            else if (arg == "--html" && i + 1 < argc) app.session->load_html(read_html(argv[++i]), "https://localhost/");
            else if (arg == "--command" && i + 1 < argc) commands.emplace_back(argv[++i]);
            else if (arg.starts_with("--")) throw std::runtime_error("Unknown option or missing value");
            else app.command("open " + arg);
        }
        app.refresh(); for (const auto& command : commands) std::cout << app.command(command) << '\n';
        if (dump) { for (const auto& line : app.page.lines) std::cout << line << '\n'; return 0; }
        if (pwtk_terminal_open() != 0) throw std::runtime_error("Cannot initialize terminal");
        struct Guard { ~Guard() { pwtk_terminal_close(); } } guard;
        std::string command, status = ":help for commands; j/k scroll; q quit", panel; bool editing = false, searching = false;
        for (;;) {
            int width = 80, height = 24; pwtk_terminal_size(&width, &height); app.refresh(static_cast<std::size_t>(std::max(10, width))); pwtk_terminal_clear();
            const auto visible = app.visible_lines(); const auto cursor = app.view == prowsetk::tui::ViewMode::Help ? app.help.cursor() : app.view == prowsetk::tui::ViewMode::Config ? app.config_cursor : app.top;
            const auto base = app.view == prowsetk::tui::ViewMode::Browser ? app.top : (cursor > static_cast<std::size_t>(std::max(1, height - 3)) ? cursor - static_cast<std::size_t>(std::max(1, height - 3)) : 0);
            paint(0, app.view == prowsetk::tui::ViewMode::Browser ? "Prowse-TUI | " + app.session->document()->title() : app.view == prowsetk::tui::ViewMode::Help ? "Help | " + app.help.title() : "Prowse-TUI Config", 36, true);
            for (int row = 1; row < height - 2; ++row) { auto index = base + static_cast<std::size_t>(row - 1); if (index < visible.size()) paint(row, (index == cursor && app.view != prowsetk::tui::ViewMode::Browser ? "> " : "") + visible[index], app.view == prowsetk::tui::ViewMode::Help ? 37 : 0, index == cursor && app.view != prowsetk::tui::ViewMode::Browser); }
            if (!panel.empty()) { std::istringstream lines(panel); std::string line; int row = 1; while (row < height - 2 && std::getline(lines, line)) paint(row++, line, 33); }
            if (height >= 2) paint(height - 2, status, 32);
            if (height >= 1) paint(height - 1, editing ? (searching ? "/" : ":") + command : "", 33);
            pwtk_terminal_present();
            PwtkEvent ev{}; if (pwtk_terminal_read(&ev) < 0) throw std::runtime_error("Terminal input failed"); if (ev.key == PWTK_KEY_CTRL_C) break;
            if (editing) {
                if (ev.key == PWTK_KEY_ESC) { editing = false; searching = false; command.clear(); }
                else if (ev.key == PWTK_KEY_ENTER) { pwtk_terminal_close(); try { if (searching) { auto count = app.help.search(command); panel = std::to_string(count) + " matches"; status = "n next, ? previous, Esc closes search"; } else { panel = app.command(command); status = "Command completed; Esc closes result"; } } catch (const std::exception& error) { panel.clear(); status = error.what(); } if (pwtk_terminal_open() != 0) throw std::runtime_error("Cannot restore terminal"); command.clear(); editing = searching = false; }
                else if (ev.key == PWTK_KEY_BACKSPACE) { if (!command.empty()) command.pop_back(); }
                else if (ev.ch && command.size() < 65532) command.push_back(static_cast<char>(ev.ch));
                continue;
            }
            if (ev.ch == ':') { editing = true; searching = false; panel.clear(); continue; }
            if (ev.ch == '/' && app.view == prowsetk::tui::ViewMode::Help) { editing = true; searching = true; command.clear(); continue; }
            if (ev.key == PWTK_KEY_ESC) { if (app.view == prowsetk::tui::ViewMode::Browser) panel.clear(); else app.close_view(); continue; }
            if (ev.ch == 'q' && app.view == prowsetk::tui::ViewMode::Browser) break;
            std::string action; if (ev.ch == 'j' || ev.key == PWTK_KEY_DOWN) action = "down"; else if (ev.ch == 'k' || ev.key == PWTK_KEY_UP) action = "up"; else if (ev.ch == 'h' || ev.key == PWTK_KEY_LEFT) action = "back"; else if (ev.ch == 'l' || ev.key == PWTK_KEY_RIGHT || ev.key == PWTK_KEY_ENTER) action = "activate"; else if (ev.ch == 'n') action = "next_match"; else if (ev.ch == '?') action = "previous_match"; else if (ev.ch == ' ' || ev.key == PWTK_KEY_PAGE_DOWN) action = "page_down"; else if (ev.ch == 'b' || ev.key == PWTK_KEY_PAGE_UP) action = "page_up"; else if (ev.ch == 'g') action = "home"; else if (ev.ch == 'G') action = "end";
            try { if (!action.empty()) { auto nested = app.key_action(action, height - 3); if (!nested.empty()) panel = app.command(nested); } } catch (const std::exception& error) { status = error.what(); }
            if (app.quit) break;
        }
        return 0;
    } catch (const std::exception& error) { std::cerr << "prowse-tui: " << error.what() << '\n'; return 1; }
}
