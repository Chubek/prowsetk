#include "prowsetk/oauth_assist.hpp"
#include "prowsetk/project_config.hpp"
#include "prowsetk/error.hpp"
#include <cstdlib>
#include <iostream>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

namespace {
void environment(std::string& value, const char* name) { if (const auto* v = std::getenv(name); v && *v) value = v; }
std::string private_input() {
    struct Terminal {
        termios saved{}; bool active = false;
        Terminal() { if (::tcgetattr(STDIN_FILENO, &saved) == 0) { auto next = saved; next.c_lflag &= static_cast<tcflag_t>(~ECHO); active = ::tcsetattr(STDIN_FILENO, TCSANOW, &next) == 0; } }
        ~Terminal() { if (active) ::tcsetattr(STDIN_FILENO, TCSANOW, &saved); }
    } terminal;
    std::string result;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(5);
    while (result.size() < 32768) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        pollfd fd{STDIN_FILENO, POLLIN, 0};
        if (remaining <= 0 || ::poll(&fd, 1, static_cast<int>(remaining)) <= 0) break;
        char byte;
        if (::read(STDIN_FILENO, &byte, 1) != 1) break;
        if (byte == '\n') return result;
        if (byte != '\r') result += byte;
    }
    throw prowsetk::Error(prowsetk::ErrorCode::Timeout, "OAuth input cancelled or expired");
}
}
int main(int argc, char** argv) {
    try {
        if (argc < 2 || std::string_view(argv[1]) == "--help") {
            std::cout << "Usage: ptk-oauth-assist login|refresh|status|logout [--config Prowse.toml]\n"
                "Uses [oauth] and PROWSETK_OAUTH_CLIENT_ID / PROWSETK_OAUTH_SCOPES.\n"
                "Login: open the printed URL, then paste the complete redirect URL (input hidden).\n";
            return argc < 2 ? 1 : 0;
        }
        prowsetk::ProjectConfig project;
        if (argc == 4 && std::string_view(argv[2]) == "--config") project = prowsetk::load_project_config(argv[3]);
        else if (argc != 2) throw prowsetk::Error(prowsetk::ErrorCode::InvalidArgument, "invalid arguments");
        else if (std::filesystem::exists("Prowse.toml")) project = prowsetk::load_project_config("Prowse.toml");
        auto& config = project.oauth;
        environment(config.client_id, "PROWSETK_OAUTH_CLIENT_ID"); environment(config.scopes, "PROWSETK_OAUTH_SCOPES");
        const auto cache = prowsetk::oauth_assist::default_cache_directory();
        const std::string command = argv[1];
        if (command == "status") {
            auto token = prowsetk::oauth_assist::load(cache, config);
            std::cout << (prowsetk::oauth_assist::expired(token) ? "expired\n" : "authenticated\n"); return 0;
        }
        if (command == "logout") { prowsetk::oauth_assist::logout(cache, config); std::cout << "Local credential removed\n"; return 0; }
        auto network = prowsetk::make_socket_network_client();
        prowsetk::oauth_assist::Token token;
        if (command == "login") {
            prowsetk::oauth_assist::Authorization flow(config);
            std::cout << "Open this authorization URL in your browser:\n" << flow.authorization_url()
                << "\nPaste the complete redirect URL within five minutes: " << std::flush;
            token = flow.finish(*network, private_input());
            std::cout << '\n';
        } else if (command == "refresh") token = prowsetk::oauth_assist::refresh(*network, config, prowsetk::oauth_assist::load(cache, config));
        else throw prowsetk::Error(prowsetk::ErrorCode::InvalidArgument, "unknown command");
        prowsetk::oauth_assist::save(cache, config, token);
        std::cout << "Authentication saved\n"; return 0;
    } catch (...) { std::cerr << "OAuth operation failed\n"; return 1; }
}
