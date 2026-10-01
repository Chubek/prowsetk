#ifndef PROWSETK_PLUGINS_SPIDER_HPP
#define PROWSETK_PLUGINS_SPIDER_HPP

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <prowsetk/browser.hpp>

namespace prowsetk::plugins::spider {

inline constexpr std::size_t max_message_bytes = 4u * 1024u * 1024u;
// Length-delimited strings, shared by IPC, durable records and the Lua bridge.
std::string pack(const std::vector<std::string>& values);
std::vector<std::string> unpack(std::string_view bytes);
std::string json_quote(std::string_view value);
bool valid_name(std::string_view name) noexcept;
std::filesystem::path default_directory();
std::string default_socket();

struct Reply {
    bool ok = true;
    std::string json = "{}";
};
Reply failure(std::string_view code);

// Transactions copy data out of LMDB before returning; no mapped pointers leak.
// Single-process owner; separate worker processes open separate environments.
class Cache {
public:
    explicit Cache(const std::filesystem::path& directory,
                   std::size_t map_bytes = 256u * 1024u * 1024u);
    ~Cache();
    Cache(const Cache&) = delete;
    Cache& operator=(const Cache&) = delete;
    std::optional<std::string> get(std::string_view key) const;
    void write(const std::vector<std::pair<std::string, std::string>>& puts,
               const std::vector<std::string>& deletes = {});
    std::vector<std::pair<std::string, std::string>> query(
        std::string_view prefix, std::size_t limit = 100,
        std::string_view after = {}) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct CrawlOptions {
    std::size_t max_pages = 1000;
    std::size_t max_depth = 5;
    std::size_t max_frontier = 10000;
    int interval_ms = 1000;
    int max_retries = 3;
};

// One thread/process owns an Engine. All traffic including script requests,
// redirects and drivers is mediated through its isolated Browser/Session.
// tick() performs at most one frontier operation; commands run at boundaries.
class Engine {
public:
    Engine(std::string name, const std::filesystem::path& directory,
           std::unique_ptr<NetworkClient> network = {});
    ~Engine();
    Reply command(const std::vector<std::string>& args);
    bool tick();
    bool running() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// One request per connection. No shell interpretation; command arguments retain
// their boundaries. Socket owner is the trust boundary. Throws on IPC failure.
Reply control(std::string_view socket, std::string_view name,
              const std::vector<std::string>& command, int timeout_ms = 45000);

} // namespace prowsetk::plugins::spider
#endif
