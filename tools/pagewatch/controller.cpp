#include "pagewatch.hpp"
#include <iostream>
#include <sstream>

int main(int argc, char** argv) {
    namespace pa = prowsetk::automation;
    namespace fs = std::filesystem;
    try {
        fs::path directory = "/var/run/pagewatch", config;
        std::vector<std::string> positional;
        std::string since = "0";
        for (int i = 1; i < argc; ++i) {
            const std::string option = argv[i];
            if (option == "--help") {
                std::cout << "usage: pgwatchctl [--directory DIR] ping|list|shutdown\n"
                    "       pgwatchctl [--directory DIR] deploy|update NAME SCRIPT --config Watcher.toml\n"
                    "       pgwatchctl [--directory DIR] status|data|pause|resume|check|remove NAME\n"
                    "       pgwatchctl [--directory DIR] updates NAME [--since SEQUENCE]\n";
                return 0;
            }
            if (option == "--directory" || option == "--config" || option == "--since") {
                if (i + 1 == argc) throw std::runtime_error("missing option value");
                const std::string value = argv[++i];
                if (option == "--directory") directory = value;
                else if (option == "--config") config = value;
                else since = value;
            } else if (option.starts_with("--")) throw std::runtime_error("unknown option");
            else positional.push_back(option);
        }
        if (positional.empty()) throw std::runtime_error("missing command");
        auto request = positional;
        if (request[0] == "deploy" || request[0] == "update") {
            if (request.size() != 3 || config.empty()) throw std::runtime_error("deploy requires name, script and configuration");
            const auto source = pa::read_file(request[2], prowsetk::pagewatch::script_limit);
            auto table = toml::parse(pa::read_file(config, prowsetk::pagewatch::script_limit));
            if (auto* watch = table["watcher"].as_table()) {
                const auto file = pa::string_setting(*watch, "html_file");
                if (!file.empty()) watch->insert_or_assign("html_file", fs::absolute(fs::absolute(config).parent_path() / file).string());
            }
            std::ostringstream normalized; normalized << table;
            prowsetk::pagewatch::parse_watch_config(normalized.str());
            request = {request[0], request[1], source, normalized.str()};
        } else if (request[0] == "updates") {
            if (request.size() != 2) throw std::runtime_error("updates requires name");
            request.push_back(since);
        }
        auto socket = pa::connect_socket(directory / "pgwatch.sock");
        pa::send_frame(socket.get(), request);
        const auto reply = pa::receive_frame(socket.get());
        if (reply.size() != 2) throw std::runtime_error("invalid daemon response");
        std::cout << reply[1] << '\n';
        return reply[0] == "ok" ? 0 : 1;
    } catch (const std::exception&) { std::cerr << "pgwatchctl: invalid command/configuration or daemon unavailable\n"; return 1; }
}
