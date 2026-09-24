// Driver-side local IPC client for Beacon Flashes.
#include "prowsetk/plugins/beacon.hpp"

#include <cerrno>
#include <cstring>
#include <iostream>
#include <string>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace beacon = prowsetk::plugins::beacon;

int main(int argc, char** argv) {
#if !defined(__unix__) && !defined(__APPLE__)
    (void)argc;
    (void)argv;
    std::cerr << "beaconctl requires Unix sockets\n";
    return 1;
#else
    std::string path = beacon::kDefaultSocketPath;
    int arg = 1;
    if (arg < argc && std::string(argv[arg]) == "--socket" && arg + 1 < argc) {
        path = argv[arg + 1];
        arg += 2;
    }
    if (arg >= argc) {
        std::cerr << "usage: beaconctl [--socket PATH] ping|list|flash TYPE URL_PATTERN|poll ID|disconnect ID\n";
        return 2;
    }
    const std::string action = argv[arg++];
    std::string request;
    if (action == "ping" && arg == argc) request = beacon::render_ping();
    else if (action == "list" && arg == argc) request = "{\"type\":\"flash_list_request\"}";
    else if (action == "flash" && arg + 2 == argc) {
        const auto type = beacon::parse_request_type(argv[arg]);
        if (!type) {
            std::cerr << "unknown Flash type\n";
            return 2;
        }
        beacon::FlashFilters filters;
        filters.url_pattern = argv[arg + 1];
        const auto flash = beacon::new_flash(*type, std::move(filters));
        if (!beacon::validate_flash_request(flash)) {
            std::cerr << "invalid Flash request\n";
            return 2;
        }
        // Wire requests retain the original pattern; redaction applies only
        // to logs and listings, otherwise URL filters could not match.
        request = beacon::render_flash_request(flash, prowsetk::Redactor(), false);
    } else if ((action == "poll" || action == "disconnect") && arg + 1 == argc) {
        request = "{\"type\":\"" +
                  std::string(action == "poll" ? "flash_poll" : "flash_disconnect") +
                  "\",\"flash_id\":\"" + beacon::json_escape(argv[arg]) + "\"}";
    } else {
        std::cerr << "invalid beaconctl arguments\n";
        return 2;
    }
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return 1;
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (path.size() >= sizeof(address.sun_path)) {
        std::cerr << "socket path too long\n";
        ::close(fd);
        return 2;
    }
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    const timeval deadline{5, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &deadline, sizeof(deadline));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &deadline, sizeof(deadline));
    if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        std::cerr << "beacond unavailable\n";
        ::close(fd);
        return 1;
    }
    request += '\n';
    std::size_t sent = 0;
    while (sent < request.size()) {
        const ssize_t n = ::send(fd, request.data() + sent, request.size() - sent, 0);
        if (n <= 0) { ::close(fd); return 1; }
        sent += static_cast<std::size_t>(n);
    }
    std::string reply;
    char buffer[4096];
    while (reply.size() <= beacon::kMaxNativeMessageBytes) {
        const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
        if (n <= 0) break;
        reply.append(buffer, static_cast<std::size_t>(n));
        if (reply.find('\n') != std::string::npos) break;
    }
    ::close(fd);
    const auto newline = reply.find('\n');
    if (newline == std::string::npos || newline > beacon::kMaxNativeMessageBytes) {
        std::cerr << "invalid beacond reply\n";
        return 1;
    }
    reply.resize(newline);
    std::cout << reply << '\n';
    return beacon::parse_message_type(reply) == "error" ? 1 : 0;
#endif
}
