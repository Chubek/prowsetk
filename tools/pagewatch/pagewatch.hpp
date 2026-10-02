#pragma once
#include "support.hpp"

namespace prowsetk::pagewatch {
inline constexpr std::size_t sample_limit = 256 * 1024;
inline constexpr std::size_t script_limit = 128 * 1024;
struct WatchConfig {
    std::string url, query, html_file;
    int interval_ms = 30000;
    int worker_timeout_ms = 60000;
    BrowserConfig browser;
    std::vector<LuaArgument> arguments;
};
WatchConfig parse_watch_config(std::string_view source);
struct DaemonConfig {
    std::filesystem::path directory = "/var/run/pagewatch";
    std::filesystem::path action_script;
    int action_timeout_ms = 5000;
    bool emit_initial = false;
};
int worker(const std::filesystem::path& directory, int ipc_fd);
int daemon(const DaemonConfig& config);
}
