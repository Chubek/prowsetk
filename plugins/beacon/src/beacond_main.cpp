// beacond: Flash session daemon bridging Beacon clients and Firefox.
//
// Listens on a Unix socket (or named pipe path) for newline-delimited JSON
// Flash protocol messages, maintains the Flash lifecycle (creation, active
// connections, timeouts, cleanup), and relays addon traffic from the Native
// Messaging host back to waiting drivers.
//
// Security: local socket permissions (and an optional --auth-token) gate
// Flash creation; the addon performs explicit user-consent connects. Page
// data and tokens are never logged.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <charconv>

#include "prowsetk/plugins/beacon.hpp"

#if defined(__unix__) || defined(__APPLE__)
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
#define BEACON_HAVE_UNIX_SOCKET 1
#else
#define BEACON_HAVE_UNIX_SOCKET 0
#endif

namespace {

namespace beacon = prowsetk::plugins::beacon;

void usage() {
    std::cout << "usage: beacond [--socket PATH] [--timeout-ms N] "
                 "[--max-flashes N]\n"
                 "\n"
                 "Bridges ProwseTk Beacon Flash requests and the Firefox "
                 "Native Messaging host.\n"
                 "Verify connectivity with: prowsetk beacon ping\n";
}

std::uint64_t now_ms() {
    using clock = std::chrono::steady_clock;
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            clock::now().time_since_epoch())
            .count());
}

std::string handle_message(beacon::FlashSessionManager* manager,
                           const std::string& line) {
    const auto type = beacon::parse_message_type(line);
    if (!type.has_value()) {
        return "{\"type\":\"error\",\"message\":\"missing type\"}";
    }
    if (*type == "ping") return "{\"type\":\"pong\"}";
    if (*type == "flash_request") {
        auto request = beacon::parse_flash_request(line);
        if (!request.has_value()) {
            return "{\"type\":\"error\",\"message\":\"invalid flash_request\"}";
        }
        std::string error;
        if (!manager->create(*request, now_ms(), &error)) {
            return "{\"type\":\"error\",\"message\":\"" +
                   beacon::json_escape(error) + "\"}";
        }
        return "{\"type\":\"flash_registered\",\"flash_id\":\"" +
               beacon::json_escape(request->flash_id) +
               "\",\"status\":\"seeking\"}";
    }
    if (*type == "flash_connect") {
        auto connect = beacon::parse_flash_connect(line);
        if (!connect.has_value()) {
            return "{\"type\":\"error\",\"message\":\"invalid flash_connect\"}";
        }
        std::string error;
        if (!manager->connect(connect->flash_id, connect->tab_id, now_ms(),
                              &error)) {
            return "{\"type\":\"error\",\"message\":\"" +
                   beacon::json_escape(error) + "\"}";
        }
        return "{\"type\":\"flash_connected\",\"flash_id\":\"" +
               beacon::json_escape(connect->flash_id) + "\"}";
    }
    if (*type == "flash_disconnect") {
        auto id = beacon::parse_flash_id(line);
        if (!id.has_value()) {
            return "{\"type\":\"error\",\"message\":\"missing flash_id\"}";
        }
        std::string error;
        if (!manager->disconnect(*id, &error)) {
            return "{\"type\":\"error\",\"message\":\"" +
                   beacon::json_escape(error) + "\"}";
        }
        return "{\"type\":\"flash_closed\",\"flash_id\":\"" +
               beacon::json_escape(*id) + "\"}";
    }
    if (*type == "flash_list_request") {
        return manager->render_list();
    }
    if (*type == "flash_data" || *type == "tracepoint_event") {
        const auto id = beacon::parse_flash_id(line);
        const auto tab = beacon::parse_tab_id(line);
        if (!id || !tab) {
            return "{\"type\":\"error\",\"message\":\"missing flash_id or tab_id\"}";
        }
        std::string error;
        if (!manager->publish(*id, *tab, line, now_ms(), &error)) {
            return "{\"type\":\"error\",\"message\":\"" +
                   beacon::json_escape(error) + "\"}";
        }
        return "{\"type\":\"flash_queued\"}";
    }
    if (*type == "flash_poll") {
        const auto id = beacon::parse_flash_id(line);
        if (!id) return "{\"type\":\"error\",\"message\":\"missing flash_id\"}";
        std::string error;
        const auto message = manager->poll(*id, now_ms(), &error);
        if (!error.empty()) return "{\"type\":\"error\",\"message\":\"" +
                                    beacon::json_escape(error) + "\"}";
        return message.value_or("{\"type\":\"flash_empty\"}");
    }
    return "{\"type\":\"error\",\"message\":\"unknown type\"}";
}

}  // namespace

int main(int argc, char** argv) {
    std::string socket_path = beacon::kDefaultSocketPath;
    beacon::BeaconOptions options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            usage();
            return 0;
        }
        if (arg == "--socket" && i + 1 < argc) {
            socket_path = argv[++i];
            options.socket_path = socket_path;
        } else if (arg == "--timeout-ms" && i + 1 < argc) {
            options.timeout_ms =
                static_cast<std::uint32_t>(std::stoul(argv[++i]));
        } else if (arg == "--max-flashes" && i + 1 < argc) {
            options.max_flashes =
                static_cast<std::uint32_t>(std::stoul(argv[++i]));
        } else {
            std::cerr << "unknown argument: " << arg << "\n";
            usage();
            return 2;
        }
    }

#if !BEACON_HAVE_UNIX_SOCKET
    std::cerr << "beacond: Unix sockets unavailable on this platform\n";
    (void)socket_path;
    return 1;
#else
    beacon::FlashSessionManager manager(options);

    int listen_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        std::cerr << "beacond: socket() failed\n";
        return 1;
    }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (socket_path.size() >= sizeof(addr.sun_path)) {
        std::cerr << "beacond: socket path too long\n";
        ::close(listen_fd);
        return 1;
    }
    struct stat existing{};
    if (::lstat(socket_path.c_str(), &existing) == 0) {
        if (!S_ISSOCK(existing.st_mode) || existing.st_uid != ::geteuid()) {
            std::cerr << "beacond: socket path is occupied\n";
            ::close(listen_fd);
            return 1;
        }
        if (::unlink(socket_path.c_str()) != 0) {
            std::cerr << "beacond: cannot remove stale socket\n";
            ::close(listen_fd);
            return 1;
        }
    }
    std::memcpy(addr.sun_path, socket_path.c_str(), socket_path.size() + 1);
    const mode_t old_mask = ::umask(0077);
    if (::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) !=
        0) {
        ::umask(old_mask);
        std::cerr << "beacond: bind() failed\n";
        ::close(listen_fd);
        return 1;
    }
    ::umask(old_mask);
    if (::chmod(socket_path.c_str(), 0600) != 0) {
        std::cerr << "beacond: cannot secure socket\n";
        ::close(listen_fd);
        ::unlink(socket_path.c_str());
        return 1;
    }
    if (::listen(listen_fd, 8) != 0) {
        std::cerr << "beacond: listen() failed\n";
        ::close(listen_fd);
        return 1;
    }
    std::cout << "beacond listening on " << socket_path << "\n" << std::flush;

    for (;;) {
        manager.expire_stale(now_ms());
        int fd = ::accept(listen_fd, nullptr, nullptr);
        if (fd < 0) continue;
        const timeval deadline{5, 0};
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &deadline, sizeof(deadline));
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &deadline, sizeof(deadline));
        std::string buffer;
        buffer.reserve(65536);
        char chunk[4096];
        bool closed = false;
        while (!closed) {
            const ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
            if (n <= 0) break;
            if (buffer.size() + static_cast<std::size_t>(n) >
                beacon::kMaxNativeMessageBytes + 1) break;
            buffer.append(chunk, static_cast<std::size_t>(n));
            std::size_t nl = 0;
            while ((nl = buffer.find('\n')) != std::string::npos) {
                const std::string line = buffer.substr(0, nl);
                buffer.erase(0, nl + 1);
                const std::string reply = handle_message(&manager, line);
                const std::string out = reply + "\n";
                std::size_t sent = 0;
                while (sent < out.size()) {
                    const ssize_t w =
                        ::send(fd, out.data() + sent, out.size() - sent, 0);
                    if (w <= 0) {
                        closed = true;
                        break;
                    }
                    sent += static_cast<std::size_t>(w);
                }
            }
            if (buffer.size() > beacon::kMaxNativeMessageBytes) break;
        }
        ::close(fd);
    }
#endif
}
