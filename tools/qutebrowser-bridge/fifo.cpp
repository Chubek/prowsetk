#include "fifo.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>

namespace prowsetk::qute {
namespace {
constexpr std::string_view magic = "QHTML1\n";
constexpr std::size_t header_size = magic.size() + 8;
constexpr std::size_t maximum = 16 * 1024 * 1024;
class Descriptor {
public:
    explicit Descriptor(int fd) : fd_(fd) {
        if (fd < 0) throw std::runtime_error("FIFO unavailable");
    }
    ~Descriptor() { ::close(fd_); }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    int get() const { return fd_; }
private:
    int fd_;
};
bool private_kind(const struct stat& info, bool directory) {
    return (directory ? S_ISDIR(info.st_mode) : S_ISFIFO(info.st_mode)) &&
        info.st_uid == ::geteuid() && !(info.st_mode & 0077);
}
}

std::string read_fifo(const std::filesystem::path& path, std::size_t size) {
    const auto name = path.filename().string();
    struct stat parent{};
    if (size > maximum || !path.is_absolute() || path.string().find('\0') != std::string::npos ||
        name.size() != 42 || !name.starts_with("bulk-") || !name.ends_with(".fifo") ||
        name.substr(5, 32).find_first_not_of("0123456789abcdef") != std::string::npos ||
        ::lstat(path.parent_path().c_str(), &parent) || !private_kind(parent, true))
        throw std::runtime_error("invalid private FIFO");
    Descriptor fd(::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
    struct stat info{};
    if (::fstat(fd.get(), &info) || !private_kind(info, false))
        throw std::runtime_error("invalid private FIFO");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    std::string frame;
    std::array<char, 65536> buffer{};
    const auto total = size + header_size;
    for (;;) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0) throw std::runtime_error("FIFO transfer timed out");
        pollfd item{fd.get(), POLLIN, 0};
        const auto ready = ::poll(&item, 1, static_cast<int>(left));
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0 || item.revents & (POLLERR | POLLNVAL))
            throw std::runtime_error("FIFO transfer failed");
        const auto remaining = total + 1 - frame.size();
        const auto count = ::read(fd.get(), buffer.data(), std::min(buffer.size(), remaining));
        if (count < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        if (count < 0) throw std::runtime_error("FIFO transfer failed");
        if (!count) {
            if (!frame.empty()) break;
            ::poll(nullptr, 0, 10);
            continue;
        }
        frame.append(buffer.data(), static_cast<std::size_t>(count));
        if (frame.size() > total) throw std::runtime_error("FIFO length exceeded");
        if (frame.size() >= header_size) {
            std::uint64_t length = 0;
            for (auto at = magic.size(); at < header_size; ++at)
                length = (length << 8) | static_cast<unsigned char>(frame[at]);
            if (!frame.starts_with(magic) || length != size)
                throw std::runtime_error("invalid FIFO frame");
        }
    }
    if (frame.size() != total) throw std::runtime_error("truncated FIFO frame");
    return frame.substr(header_size);
}
}
