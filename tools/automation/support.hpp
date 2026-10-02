#pragma once
#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>
#include <prowsetk/browser.hpp>
#include <prowsetk/lua_runtime.hpp>
#include <toml++/toml.hpp>

namespace prowsetk::automation {
inline constexpr std::size_t message_limit = 1024 * 1024;
using Clock = std::chrono::steady_clock;
class Fd {
public:
    explicit Fd(int fd = -1) : fd_(fd) {}
    ~Fd();
    Fd(Fd&& other) noexcept;
    Fd& operator=(Fd&& other) noexcept;
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    int get() const { return fd_; }
    int release();
private:
    int fd_;
};
std::string read_file(const std::filesystem::path& path, std::size_t limit = message_limit);
void private_directory(const std::filesystem::path& path);
void write_file(const std::filesystem::path& path, std::string_view data);
std::string json_quote(std::string_view value);
bool valid_name(std::string_view value);
std::string origin(std::string_view url);
void load_module(LuaRuntime& lua, std::string_view name, std::string_view source);
std::string call(LuaRuntime& lua, std::string_view name, const std::vector<LuaArgument>& args = {});
std::string string_setting(const toml::table& table, std::string_view key, std::string fallback = {});
int int_setting(const toml::table& table, std::string_view key, int fallback, int minimum, int maximum);
bool bool_setting(const toml::table& table, std::string_view key, bool fallback);
std::vector<LuaArgument> scalar_arguments(const toml::table& table);
void check_keys(const toml::table& table, const std::vector<std::string_view>& keys);
BrowserConfig browser_config(const toml::table& table, bool offline);

// Applied at the transport boundary, including redirects and script requests.
class RestrictedNetwork final : public NetworkClient {
public:
    RestrictedNetwork(std::vector<std::string> origins, std::size_t max_requests,
                      std::unique_ptr<NetworkClient> transport = make_socket_network_client());
    HttpResponse send(const HttpRequest& request) override;
private:
    std::vector<std::string> origins_;
    std::size_t remaining_;
    std::unique_ptr<NetworkClient> transport_;
};

std::string frame(const std::vector<std::string>& fields);
std::optional<std::vector<std::string>> take_frame(std::string& buffer);
void send_frame(int fd, const std::vector<std::string>& fields);
std::vector<std::string> receive_frame(int fd);
void socket_deadline(int fd, int milliseconds);
Fd connect_socket(const std::filesystem::path& path);
// argv is passed directly to execvp; never interpreted by a shell.
int run_process(const std::vector<std::string>& argv, int timeout_ms);
}
