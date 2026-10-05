#ifndef PROWSETK_CDP_HPP
#define PROWSETK_CDP_HPP

#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include "prowsetk/network_client.hpp"

namespace prowsetk {
// Host-owned message transport. Implementations enforce time and message bounds.
class WebSocket {
public:
    virtual ~WebSocket() = default;
    virtual void send(std::string_view text) = 0;
    virtual std::string receive() = 0;
};

// Synchronous CDP client, one outstanding command. Notifications are retained in
// a bounded queue. Callers serialize access; no callbacks run during a command.
// Errors never include peer-provided text. Params/result/events are JSON objects.
class CdpClient {
public:
    explicit CdpClient(std::unique_ptr<WebSocket> transport);
    ~CdpClient();
    CdpClient(const CdpClient&) = delete;
    CdpClient& operator=(const CdpClient&) = delete;
    std::string call(std::string_view method, std::string_view params = "{}",
                     std::string_view session_id = {});
    std::vector<std::string> take_events();
private:
    std::unique_ptr<WebSocket> transport_;
    unsigned long long next_id_ = 0;
    std::vector<std::string> events_;
    std::size_t event_bytes_ = 0;
    bool active_ = false;
};
} // namespace prowsetk
#endif
