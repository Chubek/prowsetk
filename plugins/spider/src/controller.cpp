#include <prowsetk/plugins/spider.hpp>
#include <fstream>
#include <iostream>
#include <iterator>

namespace spider = prowsetk::plugins::spider;
int main(int argc, char** argv) {
    try {
        std::string socket = spider::default_socket(), name;
        std::vector<std::string> command;
        bool commands = false;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (!commands && arg == "--") { commands = true; continue; }
            if (!commands && (arg == "--help" || arg == "-h")) {
                std::cout << "ptkspiderctl [--socket PATH] [--name NAME] -- COMMAND [ARGS]\n"
                    "Global: ping, list, shutdown\n"
                    "Spiders: create URL [MAX_PAGES DEPTH INTERVAL_MS], status, pause, resume, stop, remove\n"
                    "         crawl URL [MAX_PAGES DEPTH INTERVAL_MS], enqueue URL [DEPTH]\n"
                    "         watch URL SECONDS [MAX_PAGES DEPTH INTERVAL_MS], unwatch\n"
                    "         navigate URL, load-html HTML [URL], click SELECTOR [INDEX]\n"
                    "         type SELECTOR TEXT [INDEX], query SELECTOR, xpath EXPRESSION\n"
                    "         driver FILE [KEY VALUE ...]\n"
                    "         cache query [page/|record/ [LIMIT [AFTER_KEY]]]\n"
                    "         cache get KEY, cache put KEY VALUE\n"
                    "         cache select SELECTOR [PAGE_LIMIT]\n"
                    "Indexes are one-based. Drivers define main(args), returning 0.\n";
                return 0;
            }
            if (!commands && (arg == "--socket" || arg == "--name")) {
                if (++i >= argc) throw std::runtime_error("missing option value");
                (arg == "--socket" ? socket : name) = argv[i];
            } else { commands = true; command.push_back(arg); }
        }
        if (command.empty()) throw std::runtime_error("command required; use --help");
        if (command[0] == "driver" && command.size() >= 2) {
            std::ifstream file(command[1], std::ios::binary);
            if (!file) throw std::runtime_error("cannot read driver");
            std::string source;
            char bytes[4096];
            while (file.read(bytes, sizeof(bytes)) || file.gcount()) {
                source.append(bytes, static_cast<std::size_t>(file.gcount()));
                if (source.size() > 256u * 1024u) throw std::runtime_error("driver too large");
            }
            command[1] = std::move(source);
        }
        const auto reply = spider::control(socket, name, command);
        std::cout << reply.json << '\n';
        return reply.ok ? 0 : 1;
    } catch (...) {
        // Arguments and script errors can contain credentials: never echo them.
        std::cerr << "ptkspiderctl: command or IPC failed; use --help for syntax\n";
        return 2;
    }
}
