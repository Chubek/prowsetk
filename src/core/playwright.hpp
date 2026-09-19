#ifndef PROWSETK_CORE_PLAYWRIGHT_HPP
#define PROWSETK_CORE_PLAYWRIGHT_HPP

#include <memory>

#include "prowsetk/web_interface.hpp"

namespace prowsetk {

// Internal CDP bridge for Playwright. It is deliberately separate from the
// plugin system: Playwright controls a host-owned Browser and never receives
// a plugin or runtime handle.
class CdpServer {
public:
    struct Impl;

    explicit CdpServer(Browser& browser);
    ~CdpServer();

    CdpServer(const CdpServer&) = delete;
    CdpServer& operator=(const CdpServer&) = delete;

    WebResponse handle_http(const WebRequest& request);
    void handle_websocket(int fd, const WebRequest& request);

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace prowsetk

#endif  // PROWSETK_CORE_PLAYWRIGHT_HPP
