#ifndef PROWSETK_BROWSER_RUN_HPP
#define PROWSETK_BROWSER_RUN_HPP
#include "prowsetk/cdp.hpp"
#include "prowsetk/browser.hpp"
#include "prowsetk/project_config.hpp"

namespace prowsetk::browser_run {
struct Config {
    std::string account_id;
    std::string api_token;
    int timeout_ms = 30000;
    int keep_alive_ms = 60000;
};
// Environment overrides TOML; a missing API token falls back to oauth-assist's
// bound cache, refreshing only when expired. Explicit call, never plugin load.
Config resolve_config(const ProjectConfig& project, NetworkClient& network);

class Client {
public:
    Client(NetworkClient& network, Config config);
    // Acquire a browser, or reconnect to an existing browser session ID.
    std::unique_ptr<CdpClient> connect(std::string_view browser_session = {});
    // Quick Actions /content: fully rendered remote HTML, returned as caller data.
    std::string content(std::string_view url);
private:
    NetworkClient& network_;
    Config config_;
    std::string base_;
};
std::string create_page(CdpClient& client, std::string_view url);
std::string attach_page(CdpClient& client, std::string_view target_id);
std::string page_html(CdpClient& client, std::string_view session_id);
// Import into an explicitly JavaScript-disabled Flatworm Session; this transfers
// HTML only, not remote cookies, runtime state, HTTP evidence or network hooks.
void import_html(Session& session, std::string_view html, std::string_view base_url);
}
#endif
