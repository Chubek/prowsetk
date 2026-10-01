#pragma once
#include <prowsetk/plugins/spider.hpp>

namespace prowsetk::plugins::spider {
class Fd {
public:
    explicit Fd(int value = -1) : value_(value) {}
    ~Fd();
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& other) noexcept;
    Fd& operator=(Fd&& other) noexcept;
    int get() const noexcept { return value_; }
    int release() noexcept;
private:
    int value_;
};
std::string frame(const std::vector<std::string>& values);
// Incremental decoder: nullopt means incomplete. Trailing frames stay buffered.
std::optional<std::vector<std::string>> take_frame(std::string& buffer);
bool send_frame(int fd, const std::vector<std::string>& values);
std::vector<std::string> receive_frame(int fd);
void set_deadline(int fd, int timeout_ms);
}
