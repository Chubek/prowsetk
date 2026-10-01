#include "protocol.hpp"
#include <charconv>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

namespace prowsetk::plugins::spider {
Fd::~Fd() { if (value_ >= 0) ::close(value_); }
Fd::Fd(Fd&& other) noexcept : value_(other.release()) {}
Fd& Fd::operator=(Fd&& other) noexcept {
    if (this != &other) { if (value_ >= 0) ::close(value_); value_ = other.release(); }
    return *this;
}
int Fd::release() noexcept { const int result = value_; value_ = -1; return result; }

std::string pack(const std::vector<std::string>& values) {
    std::string out;
    for (const auto& value : values) {
        out += std::to_string(value.size()) + ":" + value;
        if (out.size() > max_message_bytes) throw std::runtime_error("ResourceLimit");
    }
    return out;
}
std::vector<std::string> unpack(std::string_view bytes) {
    if (bytes.size() > max_message_bytes) throw std::runtime_error("ResourceLimit");
    std::vector<std::string> out;
    while (!bytes.empty()) {
        const auto colon = bytes.find(':');
        std::size_t length = 0;
        if (colon == std::string_view::npos || colon == 0 || colon > 10)
            throw std::runtime_error("ParseError");
        const auto result = std::from_chars(bytes.data(), bytes.data() + colon, length);
        if (result.ec != std::errc{} || result.ptr != bytes.data() + colon ||
            length > bytes.size() - colon - 1 || out.size() >= 30000)
            throw std::runtime_error("ParseError");
        out.emplace_back(bytes.substr(colon + 1, length));
        bytes.remove_prefix(colon + 1 + length);
    }
    return out;
}
std::string json_quote(std::string_view value) {
    std::string out = "\"";
    constexpr char hex[] = "0123456789abcdef";
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
        else if (c < 32) { out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15]; }
        else out += static_cast<char>(c);
    }
    return out + '"';
}
Reply failure(std::string_view code) {
    std::string normalized;
    for (char c : code) {
        if (c >= 'A' && c <= 'Z') {
            if (!normalized.empty()) normalized += '_';
            normalized += static_cast<char>(c + ('a' - 'A'));
        } else normalized += c;
    }
    return {false, "{\"error\":" + json_quote(normalized) + "}"};
}
bool valid_name(std::string_view name) noexcept {
    if (name.empty() || name.size() > 80 || name.front() == '.' || name == "registry") return false;
    for (char c : name) if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) return false;
    return true;
}
std::filesystem::path default_directory() {
    if (const char* root = std::getenv("XDG_STATE_HOME")) return std::filesystem::path(root) / "prowsetk/spider";
    if (const char* home = std::getenv("HOME")) return std::filesystem::path(home) / ".local/state/prowsetk/spider";
    throw std::runtime_error("HOME or XDG_STATE_HOME required");
}
std::string default_socket() {
    if (const char* root = std::getenv("XDG_RUNTIME_DIR"))
        return (std::filesystem::path(root) / "ptkspiderd.sock").string();
    return "/tmp/ptkspiderd-" + std::to_string(::geteuid()) + ".sock";
}
std::string frame(const std::vector<std::string>& values) {
    const auto data = pack(values);
    std::string out(4, '\0');
    const auto size = static_cast<std::uint32_t>(data.size());
    for (int i = 0; i < 4; ++i) out[static_cast<std::size_t>(i)] = static_cast<char>(size >> (24 - 8 * i));
    return out + data;
}
std::optional<std::vector<std::string>> take_frame(std::string& buffer) {
    if (buffer.size() < 4) return {};
    std::uint32_t size = 0;
    for (int i = 0; i < 4; ++i) size = (size << 8) | static_cast<unsigned char>(buffer[static_cast<std::size_t>(i)]);
    if (size > max_message_bytes) throw std::runtime_error("ResourceLimit");
    if (buffer.size() < size + 4u) return {};
    auto result = unpack(std::string_view(buffer).substr(4, size));
    buffer.erase(0, size + 4u);
    return result;
}
bool send_frame(int fd, const std::vector<std::string>& values) {
    const auto data = frame(values);
    std::size_t sent = 0;
    while (sent < data.size()) {
        const auto n = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        sent += static_cast<std::size_t>(n);
    }
    return true;
}
std::vector<std::string> receive_frame(int fd) {
    std::string buffer;
    for (;;) {
        if (auto result = take_frame(buffer)) return *result;
        char bytes[4096];
        const auto n = ::recv(fd, bytes, sizeof(bytes), 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) throw std::runtime_error("IoError");
        buffer.append(bytes, static_cast<std::size_t>(n));
    }
}
void set_deadline(int fd, int timeout_ms) {
    const timeval timeout{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
}
Reply control(std::string_view socket, std::string_view name,
              const std::vector<std::string>& command, int timeout_ms) {
    Fd fd(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (fd.get() < 0 || socket.size() >= sizeof(addr.sun_path) || timeout_ms <= 0)
        throw std::runtime_error("InvalidArgument");
    std::memcpy(addr.sun_path, socket.data(), socket.size());
    set_deadline(fd.get(), timeout_ms);
    if (::connect(fd.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
        throw std::runtime_error("IoError");
    std::vector<std::string> request{std::string(name)};
    request.insert(request.end(), command.begin(), command.end());
    if (!send_frame(fd.get(), request)) throw std::runtime_error("IoError");
    const auto response = receive_frame(fd.get());
    if (response.size() != 2) throw std::runtime_error("ParseError");
    return {response[0] == "ok", response[1]};
}
}
