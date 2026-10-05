// Lua's standard library has no AF_UNIX transport. This optional module keeps
// IPC and private file I/O out of shell commands and out of page networking.
extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace {
constexpr std::size_t kMessageLimit = 8 * 1024 * 1024;
constexpr std::size_t kFileLimit = 16 * 1024 * 1024;
using Clock = std::chrono::steady_clock;

class Fd {
public:
    explicit Fd(int value) : value_(value) {
        if (value < 0) throw std::runtime_error("IPC failure");
    }
    ~Fd() { ::close(value_); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    int get() const { return value_; }
private:
    int value_;
};

void ready(int fd, short events, Clock::time_point deadline) {
    for (;;) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        if (remaining <= 0) throw std::runtime_error("IPC timeout");
        pollfd descriptor{fd, events, 0};
        const int count = ::poll(&descriptor, 1, static_cast<int>(remaining));
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0 || !(descriptor.revents & events)) throw std::runtime_error("IPC failure");
        return;
    }
}

std::string exchange(std::string_view path, std::string_view request, int timeout) {
    sockaddr_un address{};
    if (path.empty() || path.size() >= sizeof(address.sun_path) || path.find('\0') != path.npos ||
        request.empty() || request.size() > kMessageLimit || request.find_first_of("\r\n") != request.npos)
        throw std::runtime_error("invalid IPC input");
    const std::string socket_path(path);
    struct stat info{};
    if (::lstat(socket_path.c_str(), &info) || !S_ISSOCK(info.st_mode) ||
        info.st_uid != ::geteuid() || (info.st_mode & 0077))
        throw std::runtime_error("invalid IPC socket");
    Fd fd(::socket(AF_UNIX, SOCK_STREAM, 0));
    const int flags = ::fcntl(fd.get(), F_GETFL);
    if (flags < 0 || ::fcntl(fd.get(), F_SETFL, flags | O_NONBLOCK) ||
        ::fcntl(fd.get(), F_SETFD, FD_CLOEXEC))
        throw std::runtime_error("IPC failure");
#ifdef SO_NOSIGPIPE
    const int no_sigpipe = 1;
    if (::setsockopt(fd.get(), SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe)))
        throw std::runtime_error("IPC failure");
#endif
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.data(), path.size());
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout);
    if (::connect(fd.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address))) {
        if (errno != EINPROGRESS) throw std::runtime_error("IPC unavailable");
        ready(fd.get(), POLLOUT, deadline);
        int error = 0;
        socklen_t length = sizeof(error);
        if (::getsockopt(fd.get(), SOL_SOCKET, SO_ERROR, &error, &length) || error)
            throw std::runtime_error("IPC unavailable");
    }
    const std::string frame = std::string(request) + '\n';
    std::size_t at = 0;
    while (at < frame.size()) {
        ready(fd.get(), POLLOUT, deadline);
#ifdef MSG_NOSIGNAL
        constexpr int send_flags = MSG_NOSIGNAL;
#else
        constexpr int send_flags = 0;
#endif
        const auto count = ::send(fd.get(), frame.data() + at, frame.size() - at, send_flags);
        if (count < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (count <= 0) throw std::runtime_error("IPC send failed");
        at += static_cast<std::size_t>(count);
    }
    std::string result;
    char buffer[16384];
    for (;;) {
        ready(fd.get(), POLLIN, deadline);
        const auto count = ::recv(fd.get(), buffer, sizeof(buffer), 0);
        if (count < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (count <= 0 || result.size() + static_cast<std::size_t>(count) > kMessageLimit + 1)
            throw std::runtime_error("invalid IPC reply");
        result.append(buffer, static_cast<std::size_t>(count));
        const auto end = result.find('\n');
        if (end != std::string::npos) {
            if (end != result.size() - 1) throw std::runtime_error("invalid IPC frame");
            result.pop_back();
            return result;
        }
    }
}

std::string read_private(const std::string& path) {
    if (path.find('\0') != path.npos) throw std::runtime_error("invalid path");
    Fd fd(::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
    struct stat info{};
    if (::fstat(fd.get(), &info) || !S_ISREG(info.st_mode) || info.st_uid != ::geteuid() ||
        (info.st_mode & 0077) || info.st_size < 0 || static_cast<std::uint64_t>(info.st_size) > kFileLimit)
        throw std::runtime_error("invalid private file");
    std::string result;
    char buffer[16384];
    for (;;) {
        const auto count = ::read(fd.get(), buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) throw std::runtime_error("private read failed");
        if (!count) return result;
        if (result.size() + static_cast<std::size_t>(count) > kFileLimit)
            throw std::runtime_error("private file exceeds limit");
        result.append(buffer, static_cast<std::size_t>(count));
    }
}

void write_private(const std::filesystem::path& path, std::string_view bytes) {
    if (bytes.size() > kFileLimit || path.string().find('\0') != std::string::npos)
        throw std::runtime_error("invalid private output");
    // Resolve caller-selected parent aliases (a checkout may itself be a
    // symlink), while still replacing the final filename rather than following
    // a pre-existing output symlink.
    const auto absolute = std::filesystem::absolute(path);
    const auto parent = std::filesystem::weakly_canonical(absolute.parent_path());
    const auto destination = parent / absolute.filename();
    // Give newly created output directories private modes without chmod'ing
    // existing directories which may belong to the caller's project.
    std::filesystem::path part;
    for (const auto& component : parent) {
        part /= component;
        if (::mkdir(part.c_str(), 0700) && errno != EEXIST) throw std::runtime_error("output directory failed");
        struct stat info{};
        if (::lstat(part.c_str(), &info) || !S_ISDIR(info.st_mode)) throw std::runtime_error("invalid output directory");
    }
    static std::atomic<unsigned> serial{0};
    const auto temporary = destination.string() + ".qute-" + std::to_string(::getpid()) + "-" + std::to_string(++serial);
    Fd fd(::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
    try {
        std::size_t at = 0;
        while (at < bytes.size()) {
            const auto count = ::write(fd.get(), bytes.data() + at, bytes.size() - at);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) throw std::runtime_error("private write failed");
            at += static_cast<std::size_t>(count);
        }
        if (::fsync(fd.get()) || ::rename(temporary.c_str(), destination.c_str()))
            throw std::runtime_error("private output commit failed");
    } catch (...) {
        ::unlink(temporary.c_str());
        throw;
    }
}

int failure(lua_State* state) {
    lua_pushnil(state);
    lua_pushliteral(state, "qutebrowser bridge operation failed");
    return 2;
}

int lua_exchange(lua_State* state) {
    std::size_t path_size = 0, request_size = 0;
    const char* path = luaL_checklstring(state, 1, &path_size);
    const char* request = luaL_checklstring(state, 2, &request_size);
    const auto timeout = luaL_optinteger(state, 3, 32000);
    if (timeout < 1 || timeout > 32000) return failure(state);
    try {
        const auto result = exchange({path, path_size}, {request, request_size}, static_cast<int>(timeout));
        lua_pushlstring(state, result.data(), result.size());
        return 1;
    } catch (...) { return failure(state); }
}

int lua_read(lua_State* state) {
    std::size_t size = 0;
    const char* path = luaL_checklstring(state, 1, &size);
    try {
        const auto result = read_private(std::string(path, size));
        lua_pushlstring(state, result.data(), result.size());
        return 1;
    } catch (...) { return failure(state); }
}

int lua_write(lua_State* state) {
    std::size_t path_size = 0, byte_size = 0;
    const char* path = luaL_checklstring(state, 1, &path_size);
    const char* bytes = luaL_checklstring(state, 2, &byte_size);
    try {
        write_private(std::string(path, path_size), {bytes, byte_size});
        lua_pushboolean(state, true);
        return 1;
    } catch (...) { return failure(state); }
}
}  // namespace

extern "C" int luaopen_lquteipc(lua_State* state) {
    const luaL_Reg functions[] = {{"exchange", lua_exchange}, {"read_private", lua_read},
                                 {"write_private", lua_write}, {nullptr, nullptr}};
    luaL_newlib(state, functions);
    return 1;
}
