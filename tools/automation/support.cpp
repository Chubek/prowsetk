#include "support.hpp"
#include <prowsetk/error.hpp>
#include <prowsetk/url.hpp>
#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <poll.h>
#include <signal.h>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

namespace prowsetk::automation {
Fd::~Fd() { if (fd_ >= 0) ::close(fd_); }
Fd::Fd(Fd&& other) noexcept : fd_(other.release()) {}
Fd& Fd::operator=(Fd&& other) noexcept {
    if (this != &other) { if (fd_ >= 0) ::close(fd_); fd_ = other.release(); }
    return *this;
}
int Fd::release() { const int result = fd_; fd_ = -1; return result; }
std::string read_file(const std::filesystem::path& path, std::size_t limit) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("cannot read file");
    std::string result;
    char buffer[4096];
    while (stream) {
        stream.read(buffer, sizeof(buffer));
        result.append(buffer, static_cast<std::size_t>(stream.gcount()));
        if (result.size() > limit) throw std::runtime_error("file exceeds resource limit");
    }
    if (!stream.eof()) throw std::runtime_error("file read failed");
    return result;
}
void private_directory(const std::filesystem::path& path) {
    if (path.empty()) return;
    struct stat status{};
    if (::lstat(path.c_str(), &status) == 0) {
        if (!S_ISDIR(status.st_mode) || status.st_uid != ::geteuid() || (status.st_mode & 0077) != 0)
            throw std::runtime_error("directory must be owner-only and owned by this user");
        return;
    }
    if (errno != ENOENT) throw std::runtime_error("cannot inspect directory");
    // Parents may be conventional shared directories; the managed leaf is private.
    std::filesystem::create_directories(path);
    if (::chmod(path.c_str(), 0700) != 0) throw std::runtime_error("cannot protect directory");
}
void write_file(const std::filesystem::path& path, std::string_view data) {
    static std::size_t serial = 0;
    const auto temporary = path.string() + ".tmp-" + std::to_string(::getpid()) + "-" + std::to_string(++serial);
    Fd fd(::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
    if (fd.get() < 0) throw std::runtime_error("cannot create output");
    try {
        std::size_t offset = 0;
        while (offset < data.size()) {
            const auto count = ::write(fd.get(), data.data() + offset, data.size() - offset);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) throw std::runtime_error("output write failed");
            offset += static_cast<std::size_t>(count);
        }
        if (::fsync(fd.get()) != 0 || ::rename(temporary.c_str(), path.c_str()) != 0)
            throw std::runtime_error("output commit failed");
    } catch (...) { ::unlink(temporary.c_str()); throw; }
}
std::string json_quote(std::string_view value) {
    constexpr char hex[] = "0123456789abcdef";
    std::string result = "\"";
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') { result += '\\'; result += static_cast<char>(c); }
        else if (c < 32) { result += "\\u00"; result += hex[c >> 4]; result += hex[c & 15]; }
        else result += static_cast<char>(c);
    }
    return result + '"';
}
bool valid_name(std::string_view value) {
    if (value.empty() || value.size() > 64 || value.front() == '.') return false;
    return value.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-") == value.npos;
}
std::string origin(std::string_view text) {
    const auto url = parse_url(normalize_url(text));
    if (!url.userinfo.empty() || !url.has_host() || (url.scheme != "http" && url.scheme != "https"))
        throw std::runtime_error("expected an HTTP(S) URL without userinfo");
    return url.origin();
}
void load_module(LuaRuntime& lua, std::string_view name, std::string_view source) {
    const std::string code = "package.loaded[" + json_quote(name) + "] = (function()\n" + std::string(source) + "\nend)()";
    if (!lua.run(code, name).ok) throw std::runtime_error("Lua module failed");
}
std::string call(LuaRuntime& lua, std::string_view name, const std::vector<LuaArgument>& args) {
    std::string result;
    if (!lua.call_function(name, args, &result).ok) throw std::runtime_error("Lua driver failed");
    return result;
}
std::string string_setting(const toml::table& table, std::string_view key, std::string fallback) {
    if (!table.contains(key)) return fallback;
    const auto value = table[key].value<std::string>();
    if (!value) throw std::runtime_error("configuration field must be a string");
    return *value;
}
int int_setting(const toml::table& table, std::string_view key, int fallback, int minimum, int maximum) {
    if (!table.contains(key)) return fallback;
    const auto value = table[key].value<std::int64_t>();
    if (!value || *value < minimum || *value > maximum) throw std::runtime_error("configuration integer outside bounds");
    return static_cast<int>(*value);
}
bool bool_setting(const toml::table& table, std::string_view key, bool fallback) {
    if (!table.contains(key)) return fallback;
    const auto value = table[key].value<bool>();
    if (!value) throw std::runtime_error("configuration field must be boolean");
    return *value;
}
std::vector<LuaArgument> scalar_arguments(const toml::table& table) {
    std::vector<LuaArgument> result;
    for (const auto& [key, node] : table) {
        if (const auto text = node.value<std::string>()) result.push_back({std::string(key.str()), "string", *text});
        else if (const auto integer = node.value<std::int64_t>()) result.push_back({std::string(key.str()), "integer", std::to_string(*integer)});
        else if (const auto boolean = node.value<bool>()) result.push_back({std::string(key.str()), "boolean", *boolean ? "true" : "false"});
        else throw std::runtime_error("driver arguments must be scalar TOML values");
    }
    return result;
}
void check_keys(const toml::table& table, const std::vector<std::string_view>& keys) {
    for (const auto& [key, node] : table) {
        (void)node;
        if (std::find(keys.begin(), keys.end(), key.str()) == keys.end()) throw std::runtime_error("unknown configuration field");
    }
}
BrowserConfig browser_config(const toml::table& table, bool offline) {
    check_keys(table, {"javascript", "timeout_ms", "max_response_bytes", "user_agent", "proxy"});
    BrowserConfig config;
    config.javascript = bool_setting(table, "javascript", true);
    config.navigation_javascript_only = offline;
    config.timeout_ms = int_setting(table, "timeout_ms", 10000, 1, 120000);
    config.max_response_bytes = static_cast<std::size_t>(int_setting(table, "max_response_bytes", 4 * 1024 * 1024, 1, 16 * 1024 * 1024));
    config.user_agent = string_setting(table, "user_agent", config.user_agent);
    const auto proxy = string_setting(table, "proxy");
    if (!proxy.empty()) config.proxy = parse_proxy_url(proxy);
    return config;
}
RestrictedNetwork::RestrictedNetwork(std::vector<std::string> origins, std::size_t requests, std::unique_ptr<NetworkClient> transport)
    : origins_(std::move(origins)), remaining_(requests), transport_(std::move(transport)) {}
HttpResponse RestrictedNetwork::send(const HttpRequest& request) {
    if (std::find(origins_.begin(), origins_.end(), origin(request.url)) == origins_.end())
        throw Error(ErrorCode::SecurityViolation, "request outside configured origins");
    if (remaining_ == 0) throw Error(ErrorCode::ResourceLimit, "request limit reached");
    --remaining_;
    return transport_->send(request);
}
std::string frame(const std::vector<std::string>& fields) {
    std::string data;
    for (const auto& field : fields) {
        data += std::to_string(field.size()) + ":" + field;
        if (data.size() > message_limit) throw std::runtime_error("IPC message exceeds limit");
    }
    std::string result(4, '\0');
    for (int i = 0; i < 4; ++i) result[static_cast<std::size_t>(i)] = static_cast<char>(data.size() >> (24 - 8 * i));
    return result + data;
}
std::optional<std::vector<std::string>> take_frame(std::string& buffer) {
    if (buffer.size() < 4) return {};
    std::size_t size = 0;
    for (std::size_t i = 0; i < 4; ++i) size = (size << 8) | static_cast<unsigned char>(buffer[i]);
    if (size > message_limit) throw std::runtime_error("IPC message exceeds limit");
    if (buffer.size() < size + 4) return {};
    std::string_view data(buffer.data() + 4, size);
    std::vector<std::string> result;
    while (!data.empty()) {
        const auto colon = data.find(':');
        std::size_t length = 0;
        if (colon == data.npos || colon == 0 || colon > 10 || result.size() >= 32) throw std::runtime_error("invalid IPC frame");
        const auto parsed = std::from_chars(data.data(), data.data() + colon, length);
        if (parsed.ec != std::errc{} || parsed.ptr != data.data() + colon || length > data.size() - colon - 1) throw std::runtime_error("invalid IPC field");
        result.emplace_back(data.substr(colon + 1, length));
        data.remove_prefix(colon + length + 1);
    }
    buffer.erase(0, size + 4);
    return result;
}
void send_frame(int fd, const std::vector<std::string>& fields) {
    const auto data = frame(fields);
    std::size_t offset = 0;
    while (offset < data.size()) {
        int flags = 0;
#ifdef MSG_NOSIGNAL
        flags = MSG_NOSIGNAL;
#endif
        const auto count = ::send(fd, data.data() + offset, data.size() - offset, flags);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) throw std::runtime_error("IPC send failed");
        offset += static_cast<std::size_t>(count);
    }
}
std::vector<std::string> receive_frame(int fd) {
    std::string buffer;
    for (;;) {
        if (auto result = take_frame(buffer)) return *result;
        char data[4096];
        const auto count = ::recv(fd, data, sizeof(data), 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) throw std::runtime_error("IPC receive failed");
        buffer.append(data, static_cast<std::size_t>(count));
    }
}
void socket_deadline(int fd, int milliseconds) {
    const timeval deadline{milliseconds / 1000, (milliseconds % 1000) * 1000};
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &deadline, sizeof(deadline));
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &deadline, sizeof(deadline));
#ifdef SO_NOSIGPIPE
    const int enabled = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#endif
}
Fd connect_socket(const std::filesystem::path& path) {
    Fd fd(::socket(AF_UNIX, SOCK_STREAM, 0));
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const auto text = path.string();
    if (fd.get() < 0 || text.size() >= sizeof(address.sun_path)) throw std::runtime_error("invalid socket path");
    if (::fcntl(fd.get(), F_SETFD, FD_CLOEXEC) != 0) throw std::runtime_error("cannot protect socket descriptor");
    std::memcpy(address.sun_path, text.c_str(), text.size() + 1);
    socket_deadline(fd.get(), 3000);
    if (::connect(fd.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) throw std::runtime_error("daemon unavailable");
    return fd;
}
int run_process(const std::vector<std::string>& argv, int timeout_ms) {
    if (argv.empty()) throw std::runtime_error("missing process command");
    const pid_t pid = ::fork();
    if (pid < 0) throw std::runtime_error("cannot launch process");
    if (pid == 0) {
        ::setpgid(0, 0);
        std::vector<char*> args;
        for (const auto& arg : argv) args.push_back(const_cast<char*>(arg.c_str()));
        args.push_back(nullptr);
        ::execvp(args.front(), args.data());
        ::_exit(127);
    }
    ::setpgid(pid, pid);
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    int status = 0;
    for (;;) {
        const auto result = ::waitpid(pid, &status, WNOHANG);
        if (result == pid) return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
        if (result < 0 && errno != EINTR) throw std::runtime_error("cannot wait for process");
        if (Clock::now() >= deadline) { ::kill(-pid, SIGKILL); while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {} return 124; }
        ::poll(nullptr, 0, 10);
    }
}
}
