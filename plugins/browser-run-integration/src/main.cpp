#include "prowsetk/browser_run.hpp"
#include "prowsetk/error.hpp"
#include <fstream>
#include <iostream>
#include <map>
#ifdef __unix__
#include <fcntl.h>
#include <unistd.h>
#endif

int main(int argc, char** argv) {
    try {
        if (argc < 2 || std::string_view(argv[1]) == "--help") {
            std::cout << "Usage: ptk-browser-run content --url URL --output FILE [--config Prowse.toml]\n"
                "       ptk-browser-run cdp --method DOMAIN.METHOD [--params JSON] [--session ID]\n"
                "           [--browser-session ID] [--output FILE] [--config Prowse.toml]\n"
                "Credentials: CLOUDFLARE_ACCOUNT_ID / CLOUDFLARE_API_TOKEN or [cloudflare].\n"
                "CDP JSON is written only with --output; it may contain private page data.\n";
            return argc < 2 ? 1 : 0;
        }
        std::map<std::string, std::string> args;
        for (int i = 2; i < argc; i += 2) {
            const std::string key = argv[i];
            if (i + 1 >= argc || (key != "--url" && key != "--output" && key != "--config" && key != "--method" && key != "--params" && key != "--session" && key != "--browser-session") || !args.emplace(key, argv[i + 1]).second)
                throw prowsetk::Error(prowsetk::ErrorCode::InvalidArgument, "invalid arguments");
        }
        prowsetk::ProjectConfig project;
        if (args.contains("--config")) project = prowsetk::load_project_config(args["--config"]);
        else if (std::filesystem::exists("Prowse.toml")) project = prowsetk::load_project_config("Prowse.toml");
        auto network = prowsetk::make_socket_network_client();
        prowsetk::browser_run::Client client(*network, prowsetk::browser_run::resolve_config(project, *network));
        std::string result;
        if (std::string_view(argv[1]) == "content" && args.contains("--url") && args.contains("--output")) result = client.content(args["--url"]);
        else if (std::string_view(argv[1]) == "cdp" && args.contains("--method")) {
            auto cdp = client.connect(args["--browser-session"]);
            result = cdp->call(args["--method"], args.contains("--params") ? args["--params"] : "{}", args["--session"]);
        } else throw prowsetk::Error(prowsetk::ErrorCode::InvalidArgument, "invalid command");
        if (args.contains("--output")) {
#ifdef __unix__
            // Remote HTML/CDP may contain secrets. Create an owner-only new file
            // and never follow a symlink or overwrite an existing export.
            const int fd = ::open(args["--output"].c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
            if (fd < 0) throw prowsetk::Error(prowsetk::ErrorCode::IoError, "output creation failed");
            bool failed = false;
            std::size_t offset = 0;
            while (offset < result.size()) {
                const auto n = ::write(fd, result.data() + offset, result.size() - offset);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) { failed = true; break; }
                offset += static_cast<std::size_t>(n);
            }
            if (::close(fd)) failed = true;
            if (failed) { ::unlink(args["--output"].c_str()); throw prowsetk::Error(prowsetk::ErrorCode::IoError, "output failed"); }
#else
            throw prowsetk::Error(prowsetk::ErrorCode::Unsupported, "private output requires POSIX");
#endif
        }
        std::cout << "Browser Run operation completed\n";
        return 0;
    } catch (...) { std::cerr << "Browser Run operation failed\n"; return 1; }
}
