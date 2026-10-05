#include <gtest/gtest.h>
#include "prowsetk/browser_run.hpp"
#include <deque>
#include <cstdlib>
using namespace prowsetk;
namespace {
struct Peer : WebSocket {
    std::deque<std::string> replies{
        R"({"id":1,"result":{"targetId":"remote-target"}})",
        R"({"id":2,"result":{"sessionId":"remote-session"}})",
        R"({"id":3,"sessionId":"remote-session","result":{"result":{"type":"string","value":"<h1>Cloud snapshot</h1><script>fetch('/never')</script><a href='/api/items'>API</a>"}}})"};
    void send(std::string_view) override {}
    std::string receive() override { auto text = replies.front(); replies.pop_front(); return text; }
};
}
TEST(BrowserRunIntegration, RemoteCdpToFlatwormWithoutPageNetwork) {
    CdpClient client(std::make_unique<Peer>());
    auto target = browser_run::create_page(client, "https://example.test/");
    auto remote = browser_run::attach_page(client, target);
    auto html = browser_run::page_html(client, remote);
    BrowserConfig config; config.javascript = false;
    Browser browser(config);
    auto network = std::make_unique<MemoryNetworkClient>(); auto* observed = network.get();
    browser.set_network_client(std::move(network)); auto session = browser.create_session();
    browser_run::import_html(*session, html, "https://example.test/");
    EXPECT_EQ(session->document()->query_selector("h1")->text(), "Cloud snapshot");
    EXPECT_EQ(session->document()->query_selector("a")->attribute("href"), "/api/items");
    EXPECT_TRUE(observed->requests().empty());
}
TEST(BrowserRunIntegration, PluginLoadingIsNetworkFree) {
    Browser browser;
    auto network = std::make_unique<MemoryNetworkClient>(); auto* observed = network.get();
    browser.set_network_client(std::move(network));
    browser.plugins().load_native(BROWSER_RUN_PLUGIN);
    browser.plugins().load_native(OAUTH_ASSIST_PLUGIN);
    EXPECT_TRUE(observed->requests().empty());
}
#ifdef __unix__
TEST(BrowserRunIntegration, ExpiredOAuthCacheRefreshesThroughHost) {
    struct Environment {
        std::string key, value; bool existed;
        Environment(const char* name, const std::string& replacement) : key(name), existed(std::getenv(name) != nullptr) {
            if (existed) value = std::getenv(name);
            ::setenv(name, replacement.c_str(), 1);
        }
        ~Environment() { if (existed) ::setenv(key.c_str(), value.c_str(), 1); else ::unsetenv(key.c_str()); }
    };
    const auto home = std::filesystem::canonical(TEST_BINARY_DIR) / "browser-run-oauth-home";
    std::filesystem::remove_all(home);
    Environment h("HOME", home.string()), a("CLOUDFLARE_API_TOKEN", ""), b("CLOUDFLARE_ACCOUNT_ID", ""),
        c("PROWSETK_OAUTH_CLIENT_ID", ""), d("PROWSETK_OAUTH_SCOPES", "");
    ProjectConfig project; project.cloudflare_account_id = std::string(32, 'a');
    project.oauth.client_id = "fixture-client"; project.oauth.scopes = "fixture-scope";
    const auto cache = oauth_assist::default_cache_directory();
    oauth_assist::save(cache, project.oauth, {"old-access", "old-refresh", 1});
    MemoryNetworkClient network;
    network.set_handler([](const HttpRequest& request) {
        EXPECT_EQ(request.url, "https://dash.cloudflare.com/oauth2/token");
        EXPECT_EQ(request.method, "POST");
        EXPECT_NE(request.body.find("refresh_token=old-refresh"), std::string::npos);
        return HttpResponse{200, {}, R"({"access_token":"new-access","refresh_token":"new-refresh","token_type":"Bearer","expires_in":3600})", {}, {}};
    });
    const auto resolved = browser_run::resolve_config(project, network);
    EXPECT_EQ(resolved.api_token, "new-access");
    EXPECT_EQ(oauth_assist::load(cache, project.oauth).refresh_token, "new-refresh");
    (void)browser_run::resolve_config(project, network);
    EXPECT_EQ(network.requests().size(), 1u);
    std::filesystem::remove_all(home);
}
#endif
