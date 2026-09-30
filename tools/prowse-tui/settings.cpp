#include "settings.hpp"
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <set>
#include <stdexcept>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#ifdef PROWSETK_TUI_TOML
#include <toml.hpp>
#endif
namespace prowsetk::tui {
namespace {
void valid_path(const std::string& path) {
    if (path.empty() || path.size() > 4096 || path.find('\0') != std::string::npos || path.find('\n') != std::string::npos || path.find('\r') != std::string::npos)
        throw std::runtime_error("Extension paths must contain 1..4096 bytes without line breaks");
}
bool valid_key(const std::string& key) {
    if (key.size() == 1) return key[0] >= 33 && key[0] <= 126 && key != ":";
    return key == "Space" || key == "Tab" || key == "F1" || key == "F2" || key == "F3" || key == "F4" || key == "F5" || key == "F6" || key == "F7" || key == "F8" || key == "F9" || key == "F10" || key == "F11" || key == "F12";
}
}
std::string Settings::action(std::string_view key) const {
    for (const auto& [name, binding] : keys) if (binding == key) return name;
    return {};
}
void Settings::validate() const {
    const Settings defaults;
    if (keys.size() != defaults.keys.size()) throw std::runtime_error("Unknown or missing keybinding action");
    std::set<std::string> seen;
    for (const auto& [name, key] : keys) {
        if (!defaults.keys.contains(name) || !valid_key(key)) throw std::runtime_error("Invalid action or key; consult :help open config");
        if (!seen.insert(key).second) throw std::runtime_error("Duplicate keybinding; choose an unused key");
    }
    for (const auto* paths : {&lua_extensions, &native_plugins}) {
        if (paths->size() > 64) throw std::runtime_error("At most 64 extensions of each kind are allowed");
        seen.clear();
        for (const auto& path : *paths) { valid_path(path); if (!seen.insert(path).second) throw std::runtime_error("Duplicate extension path"); }
    }
}
void Settings::bind(std::string_view name, std::string_view key) {
    if (!keys.contains(std::string(name))) throw std::runtime_error("Unknown keybinding action");
    Settings next = *this; next.keys[std::string(name)] = key; next.validate(); *this = std::move(next);
}
std::filesystem::path settings_path(std::string_view xdg, std::string_view home) {
    std::filesystem::path base{xdg};
    if (base.empty() || !base.is_absolute()) {
        base = std::filesystem::path(home);
        if (base.empty() || !base.is_absolute()) throw std::runtime_error("An absolute XDG_CONFIG_HOME or HOME is required");
        base /= ".config";
    }
    return base / "prowse" / "ProwseTUI.toml";
}
std::filesystem::path default_settings_path() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    const char* home = std::getenv("HOME");
    return settings_path(xdg ? xdg : "", home ? home : "");
}
Settings parse_settings(std::string_view text) {
    if (text.size() > 256 * 1024) throw std::runtime_error("Config exceeds 256 KiB");
#ifdef PROWSETK_TUI_TOML
    toml::table root;
    try { root = toml::parse(text); }
    catch (const toml::parse_error&) { throw std::runtime_error("Invalid TOML in ProwseTUI.toml"); }
    Settings result;
    for (const auto& [key, value] : root) {
        const auto name = key.str();
        if (name == "keys") {
            const auto* table = value.as_table();
            if (!table) throw std::runtime_error("keys must be a TOML table");
            for (const auto& [action, node] : *table) {
                auto string = node.value<std::string>();
                if (!string || !result.keys.contains(std::string(action.str()))) throw std::runtime_error("Unknown action or invalid keybinding type");
                result.keys[std::string(action.str())] = *string;
            }
        } else if (name == "proxy") {
            auto string = value.value<std::string>();
            if (!string) throw std::runtime_error("proxy must be a string");
            result.proxy = *string;
        } else if (name == "lua_extensions" || name == "native_plugins") {
            const auto* array = value.as_array();
            if (!array) throw std::runtime_error("Extensions must be arrays of paths");
            auto& paths = name == "lua_extensions" ? result.lua_extensions : result.native_plugins;
            for (const auto& node : *array) {
                auto path = node.value<std::string>();
                if (!path) throw std::runtime_error("Extension path must be a string");
                paths.push_back(*path);
            }
        } else throw std::runtime_error("Unknown ProwseTUI.toml setting");
    }
    result.validate(); return result;
#else
    (void)text;
    throw std::runtime_error("TOML configuration unavailable in this build");
#endif
}
std::string encode_settings(const Settings& settings) {
    settings.validate();
#ifdef PROWSETK_TUI_TOML
    toml::table root, keys;
    for (const auto& [name, key] : settings.keys) keys.insert(name, key);
    toml::array lua, native;
    for (const auto& path : settings.lua_extensions) lua.push_back(path);
    for (const auto& path : settings.native_plugins) native.push_back(path);
    root.insert("keys", std::move(keys)); root.insert("lua_extensions", std::move(lua)); root.insert("native_plugins", std::move(native));
    if (!settings.proxy.empty()) root.insert("proxy", settings.proxy);
    std::ostringstream out;
    out << "# Prowse-TUI configuration. Extensions load on the next launch.\n" << root << '\n';
    return out.str();
#else
    throw std::runtime_error("TOML configuration unavailable in this build");
#endif
}
Settings load_settings(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) return {};
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Cannot read ProwseTUI.toml");
    std::string text;
    char chunk[8192];
    while (file.read(chunk, sizeof(chunk)) || file.gcount()) {
        text.append(chunk, static_cast<std::size_t>(file.gcount()));
        if (text.size() > 256 * 1024) throw std::runtime_error("Config exceeds 256 KiB");
    }
    if (!file.eof()) throw std::runtime_error("Cannot read ProwseTUI.toml");
    return parse_settings(text);
}
void save_settings(const std::filesystem::path& path, const Settings& settings) {
    const auto text = encode_settings(settings);
    if (path.empty()) throw std::runtime_error("Config path is unavailable");
    std::filesystem::create_directories(path.parent_path());
    auto pattern = (path.parent_path() / ".ProwseTUI.toml.XXXXXX").string();
    std::vector<char> name(pattern.begin(), pattern.end()); name.push_back('\0');
    const int fd = mkstemp(name.data()); // Exclusive, mode 0600, same filesystem.
    if (fd < 0) throw std::runtime_error("Cannot create temporary config");
    struct Temp {
        int fd; const char* path;
        ~Temp() { if (fd >= 0) ::close(fd); ::unlink(path); }
    } temp{fd, name.data()};
    std::size_t offset = 0;
    while (offset < text.size()) {
        auto n = ::write(fd, text.data() + offset, text.size() - offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) throw std::runtime_error("Cannot write temporary config");
        offset += static_cast<std::size_t>(n);
    }
    if (::fsync(fd) != 0) throw std::runtime_error("Cannot flush config");
    const auto close_result = ::close(fd); temp.fd = -1;
    if (close_result != 0) throw std::runtime_error("Cannot close config");
    std::filesystem::rename(name.data(), path);
}
std::filesystem::path extension_path(const std::filesystem::path& config, const std::string& path) {
    const std::filesystem::path value(path);
    return value.is_absolute() ? value : config.parent_path() / value;
}
}
