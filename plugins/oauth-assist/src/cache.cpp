#include "prowsetk/oauth_assist.hpp"
#include "protocol_json.hpp"
#include <cstdlib>
#include <cmath>
#ifdef __unix__
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace prowsetk::oauth_assist {
using namespace protocol;
std::filesystem::path default_cache_directory() {
    const auto* home = std::getenv("HOME");
    if (!home || !*home) throw Error(ErrorCode::StorageError, "OAuth cache requires HOME");
    return std::filesystem::path(home) / ".cache/ProwseTk/OAuth";
}
namespace {
void identity(Json& json, const Config& config) {
    put(json, "issuer", config.token_endpoint); put(json, "client", config.client_id);
    put(json, "redirect", config.redirect_uri); put(json, "scopes", config.scopes);
}
#ifdef __unix__
struct Fd {
    int value;
    explicit Fd(int fd) : value(fd) { if (fd < 0) throw Error(ErrorCode::StorageError, "OAuth cache unavailable"); }
    ~Fd() { ::close(value); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
};
int open_directory(const std::filesystem::path& path) {
    if (!path.is_absolute()) throw Error(ErrorCode::InvalidArgument, "OAuth cache path must be absolute");
    Fd parent(::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    for (const auto& part : path.relative_path()) {
        if (part == ".." || part == "." || part.empty()) throw Error(ErrorCode::InvalidArgument, "invalid OAuth cache path");
        if (::mkdirat(parent.value, part.c_str(), 0700) != 0 && errno != EEXIST) throw Error(ErrorCode::StorageError, "OAuth cache directory failed");
        Fd child(::openat(parent.value, part.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        std::swap(parent.value, child.value);
    }
    struct stat status{};
    if (::fstat(parent.value, &status) || status.st_uid != ::geteuid() || (status.st_mode & 077))
        throw Error(ErrorCode::SecurityViolation, "OAuth cache directory must be owner-only");
    return ::fcntl(parent.value, F_DUPFD_CLOEXEC, 0);
}
#endif
}
void save(const std::filesystem::path& directory, const Config& config, const Token& token) {
#ifdef __unix__
    if (token.access_token.empty() || token.access_token.size() > 16384 || token.refresh_token.size() > 16384 || token.expires_at <= 0 || token.expires_at > 9007199254740991LL)
        throw Error(ErrorCode::InvalidArgument, "invalid OAuth token");
    auto json = Json::object_value(); identity(json, config);
    put(json, "access_token", token.access_token); put(json, "refresh_token", token.refresh_token);
    json.put("expires_at", Json::number_value(static_cast<double>(token.expires_at)));
    const auto bytes = encode(json);
    if (bytes.size() > 65536) throw Error(ErrorCode::ResourceLimit, "OAuth cache limit");
    Fd dir(open_directory(directory));
    // Exclusive temp creation prevents concurrent writers from sharing a file.
    const auto temp = ".token-" + std::to_string(::getpid());
    Fd file(::openat(dir.value, temp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
    try {
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const auto n = ::write(file.value, bytes.data() + offset, bytes.size() - offset);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) throw Error(ErrorCode::StorageError, "OAuth cache write failed");
            offset += static_cast<std::size_t>(n);
        }
        if (::fsync(file.value) || ::renameat(dir.value, temp.c_str(), dir.value, "token.json") || ::fsync(dir.value))
            throw Error(ErrorCode::StorageError, "OAuth cache commit failed");
    } catch (...) { ::unlinkat(dir.value, temp.c_str(), 0); throw; }
#else
    (void)directory; (void)config; (void)token;
    throw Error(ErrorCode::Unsupported, "OAuth cache requires POSIX");
#endif
}
Token load(const std::filesystem::path& directory, const Config& config) {
#ifdef __unix__
    Fd dir(open_directory(directory));
    Fd file(::openat(dir.value, "token.json", O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
    struct stat status{};
    if (::fstat(file.value, &status) || !S_ISREG(status.st_mode) || status.st_uid != ::geteuid() ||
        (status.st_mode & 077) || status.st_nlink != 1 || status.st_size < 1 || status.st_size > 65536)
        throw Error(ErrorCode::SecurityViolation, "invalid OAuth cache permissions or size");
    std::string bytes(static_cast<std::size_t>(status.st_size), '\0');
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto n = ::read(file.value, bytes.data() + offset, bytes.size() - offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) throw Error(ErrorCode::StorageError, "OAuth cache read failed");
        offset += static_cast<std::size_t>(n);
    }
    const auto json = parse(bytes);
    if (string(json, "issuer") != config.token_endpoint || string(json, "client") != config.client_id ||
        string(json, "redirect") != config.redirect_uri || string(json, "scopes") != config.scopes)
        throw Error(ErrorCode::SecurityViolation, "OAuth cache configuration mismatch");
    const auto* expires = json.find("expires_at");
    if (!expires || expires->kind != Json::Kind::Number || expires->number <= 0 || expires->number > 9007199254740991.0 || std::floor(expires->number) != expires->number)
        throw Error(ErrorCode::ParseError, "invalid OAuth cache expiry");
    return {string(json, "access_token"), string(json, "refresh_token"), static_cast<std::int64_t>(expires->number)};
#else
    (void)directory; (void)config;
    throw Error(ErrorCode::Unsupported, "OAuth cache requires POSIX");
#endif
}
void logout(const std::filesystem::path& directory, const Config& config) {
    (void)load(directory, config);
#ifdef __unix__
    Fd dir(open_directory(directory));
    if (::unlinkat(dir.value, "token.json", 0)) throw Error(ErrorCode::StorageError, "OAuth logout failed");
#endif
}
}
