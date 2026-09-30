#pragma once
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>
namespace prowsetk::tui {
struct Settings {
    // Action -> primary key. Arrow keys, Enter, Esc and ':' are always available.
    std::map<std::string, std::string> keys{
        {"down", "j"}, {"up", "k"}, {"back", "h"}, {"activate", "l"},
        {"page_down", "Space"}, {"page_up", "b"}, {"home", "g"}, {"end", "G"},
        {"close", "q"}, {"search", "/"}, {"next_match", "n"}, {"previous_match", "?"},
        {"help", "F1"}, {"config", "F2"}};
    std::vector<std::string> lua_extensions;
    std::vector<std::string> native_plugins;
    std::string proxy;
    std::string action(std::string_view key) const;
    void validate() const;
    void bind(std::string_view action, std::string_view key);
};
std::filesystem::path settings_path(std::string_view xdg, std::string_view home);
std::filesystem::path default_settings_path();
Settings parse_settings(std::string_view text);
std::string encode_settings(const Settings& settings);
Settings load_settings(const std::filesystem::path& path);
void save_settings(const std::filesystem::path& path, const Settings& settings);
std::filesystem::path extension_path(const std::filesystem::path& config, const std::string& path);
}
