#include "pagewatch.hpp"
#include <iostream>

int main(int argc, char** argv) {
    namespace pa = prowsetk::automation;
    try {
        if (argc == 5 && std::string(argv[1]) == "--worker" && std::string(argv[3]) == "--ipc-fd")
            return prowsetk::pagewatch::worker(argv[2], std::stoi(argv[4]));
        prowsetk::pagewatch::DaemonConfig config;
        std::filesystem::path config_path;
        std::string directory;
        for (int i = 1; i < argc; ++i) {
            const std::string option = argv[i];
            if (option == "--help") { std::cout << "usage: pgwatchd [--config Pagewatch.toml] [--directory DIR]\n"; return 0; }
            if (i + 1 == argc) throw std::runtime_error("missing option value");
            if (option == "--config") config_path = argv[++i];
            else if (option == "--directory") directory = argv[++i];
            else throw std::runtime_error("unknown option");
        }
        if (!config_path.empty()) {
            const auto document = toml::parse(pa::read_file(config_path));
            pa::check_keys(document, {"pagewatch"});
            const auto* table = document["pagewatch"].as_table();
            if (!table) throw std::runtime_error("missing [pagewatch] table");
            pa::check_keys(*table, {"directory", "action_script", "action_timeout_ms", "emit_initial"});
            config.directory = pa::string_setting(*table, "directory", config.directory.string());
            config.action_script = pa::string_setting(*table, "action_script");
            if (!config.action_script.empty() && config.action_script.is_relative()) config.action_script = std::filesystem::absolute(config_path).parent_path() / config.action_script;
            config.action_timeout_ms = pa::int_setting(*table, "action_timeout_ms", 5000, 10, 300000);
            config.emit_initial = pa::bool_setting(*table, "emit_initial", false);
        }
        if (!directory.empty()) config.directory = directory;
        config.directory = std::filesystem::absolute(config.directory);
        return prowsetk::pagewatch::daemon(config);
    } catch (const std::exception&) { std::cerr << "pgwatchd: configuration, runtime directory or daemon failure\n"; return 1; }
}
