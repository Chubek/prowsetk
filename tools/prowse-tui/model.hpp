#pragma once
#include <prowsetk/browser.hpp>
#include <prowsetk/ir.hpp>
#include <string>
#include <vector>
#include "help.hpp"
#include "settings.hpp"
namespace prowsetk::tui {
struct Page {
    std::vector<std::string> lines;
    std::vector<std::string> links;
};
Page render(const ProwseEventStream& events, std::string_view base, std::size_t width);
std::string safe_text(std::string_view text);
enum class ViewMode { Browser, Help, Config };
struct ConfigRow { std::string text; std::string command; };
class Controller {
public:
    Controller();
    Browser browser;
    std::shared_ptr<Session> session;
    Page page;
    std::size_t top = 0;
    bool quit = false;
    ViewMode view = ViewMode::Browser;
    HelpPager help;
    Settings settings;
    Settings draft_settings;
    std::filesystem::path config_path;
    bool config_dirty = false;
    std::size_t config_cursor = 0;
    // Called once at application startup, after binding the live session.
    void initialize_settings(const std::filesystem::path& path);
    std::vector<ConfigRow> config_rows() const;
    std::vector<std::string> visible_lines() const;
    void close_view();
    std::string key_action(std::string_view action, int page_size = 20);
    std::string activate_view();
    void refresh(std::size_t width = 80);
    std::string command(std::string_view input);
private:
    std::vector<std::string> history_;
    std::vector<std::string> bookmarks_;
    std::size_t width_ = 80;
    void navigate(std::string_view url);
    std::string config_command(std::string_view input);
};
}
