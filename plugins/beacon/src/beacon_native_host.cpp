// beacon-native-host: Firefox Native Messaging bridge to beacond.
//
// Reads stdio Native Messaging frames (4-byte LE length + JSON) from the
// ProwseTk Beacon addon, forwards each frame over the beacond Unix socket as
// newline-delimited JSON, and writes the daemon reply back as a framed
// message. The allowlist (manifest `allowed_extensions`) restricts Firefox to
// the official addon id; this binary never accesses pages directly.
//
// The transport is local IPC only: no TCP, no WASI, no arbitrary sockets.
// Secrets (auth tokens, cookies, page HTML) pass through verbatim and are
// never logged.

#include <arpa/inet.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "prowsetk/plugins/beacon.hpp"

#if defined(__unix__) || defined(__APPLE__)
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
#define BEACON_HAVE_UNIX_SOCKET 1
#else
#define BEACON_HAVE_UNIX_SOCKET 0
#endif

namespace {

void usage() {
    std::cout << "usage: beacon-native-host [--socket PATH]\n"
                 "\n"
                 "Firefox Native Messaging host for the ProwseTk Beacon addon.\n"
                 "Reads framed JSON on stdin, forwards it to beacond over IPC,\n"
                 "and writes the framed reply to stdout.\n";
}

bool read_exact(std::string* out, std::size_t count) {
    out->resize(count);
    std::size_t done = 0;
    while (done < count) {
        std::cin.read(out->data() + done,
                      static_cast<std::streamsize>(count - done));
        const std::streamsize got = std::cin.gcount();
        if (got <= 0) return false;
        done += static_cast<std::size_t>(got);
    }
    return true;
}

#if BEACON_HAVE_UNIX_SOCKET
bool forward_to_beacond(const std::string& socket_path, const std::string& json,
                        std::string* reply, std::string* error) {
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        if (error != nullptr) *error = "socket() failed";
        return false;
    }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (socket_path.size() >= sizeof(addr.sun_path)) {
        ::close(fd);
        if (error != nullptr) *error = "socket path too long";
        return false;
    }
    std::memcpy(addr.sun_path, socket_path.c_str(), socket_path.size() + 1);
    const timeval deadline{5, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &deadline, sizeof(deadline));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &deadline, sizeof(deadline));
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        if (error != nullptr) *error = "connect to beacond failed";
        return false;
    }
    const std::string line = json + "\n";
    std::size_t sent = 0;
    while (sent < line.size()) {
        const ssize_t n =
            ::send(fd, line.data() + sent, line.size() - sent, 0);
        if (n <= 0) {
            ::close(fd);
            if (error != nullptr) *error = "send to beacond failed";
            return false;
        }
        sent += static_cast<std::size_t>(n);
    }
    // Read one newline-terminated reply with a bounded buffer.
    std::string out;
    out.reserve(65536);
    std::array<char, 4096> buf{};
    while (out.size() < prowsetk::plugins::beacon::kMaxNativeMessageBytes) {
        const ssize_t n = ::recv(fd, buf.data(), buf.size(), 0);
        if (n <= 0) break;
        if (out.size() + static_cast<std::size_t>(n) >
            prowsetk::plugins::beacon::kMaxNativeMessageBytes + 1) break;
        out.append(buf.data(), static_cast<std::size_t>(n));
        if (out.find('\n') != std::string::npos) break;
    }
    ::close(fd);
    const std::size_t nl = out.find('\n');
    if (nl == std::string::npos || nl >
        prowsetk::plugins::beacon::kMaxNativeMessageBytes) {
        if (error != nullptr) *error = "invalid reply from beacond";
        return false;
    }
    if (reply != nullptr) *reply = out.substr(0, nl);
    return true;
}
#endif

}  // namespace

int main(int argc, char** argv) {
    std::string socket_path =
        prowsetk::plugins::beacon::kDefaultSocketPath;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            usage();
            return 0;
        }
        if (arg == "--socket" && i + 1 < argc) {
            socket_path = argv[++i];
        } else {
            std::cerr << "unknown argument: " << arg << "\n";
            usage();
            return 2;
        }
    }

#if !BEACON_HAVE_UNIX_SOCKET
    std::cerr << "beacon-native-host: Unix sockets unavailable on this "
                 "platform\n";
    (void)socket_path;
    return 1;
#else
    // Disable sync-with-stdio quirks for binary framing; keep byte-exact I/O.
    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);

    for (;;) {
        std::string prefix;
        if (!read_exact(&prefix, 4)) break;  // clean EOF from Firefox
        const auto b0 = static_cast<std::uint8_t>(prefix[0]);
        const auto b1 = static_cast<std::uint8_t>(prefix[1]);
        const auto b2 = static_cast<std::uint8_t>(prefix[2]);
        const auto b3 = static_cast<std::uint8_t>(prefix[3]);
        const std::uint32_t len =
            static_cast<std::uint32_t>(b0) |
            (static_cast<std::uint32_t>(b1) << 8) |
            (static_cast<std::uint32_t>(b2) << 16) |
            (static_cast<std::uint32_t>(b3) << 24);
        if (len > prowsetk::plugins::beacon::kMaxNativeMessageBytes ||
            len == 0) {
            break;
        }
        std::string json;
        if (!read_exact(&json, len)) break;

        std::string reply;
        std::string error;
        if (!forward_to_beacond(socket_path, json, &reply, &error)) {
            reply = "{\"type\":\"error\",\"message\":\"beacond unavailable\"}";
        }
        const std::vector<std::uint8_t> framed =
            prowsetk::plugins::beacon::encode_native_message(reply);
        if (framed.empty()) break;
        std::cout.write(reinterpret_cast<const char*>(framed.data()),
                        static_cast<std::streamsize>(framed.size()));
        std::cout.flush();
        if (!std::cout.good()) break;
    }
    return 0;
#endif
}
