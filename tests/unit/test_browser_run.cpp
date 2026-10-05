#include <gtest/gtest.h>
#include "prowsetk/browser_run.hpp"
#include "prowsetk/error.hpp"
#include <cstdlib>
using namespace prowsetk;
namespace br = prowsetk::browser_run;
namespace {
br::Config config() { return {std::string(32, 'a'), "fixture-token", 30000, 60000}; }
struct Wire : WebSocket {
    std::string command;
    void send(std::string_view value) override { command = value; }
    std::string receive() override { return R"({"id":1,"result":{"product":"remote"}})"; }
};
struct Network : MemoryNetworkClient {
    HttpRequest upgrade;
    std::unique_ptr<WebSocket> open_websocket(const HttpRequest& request) override {
        upgrade = request; return std::make_unique<Wire>();
    }
};
}
TEST(BrowserRun, AuthenticatedUpgradeAndReconnect) {
    Network network; br::Client client(network, config());
    auto cdp = client.connect();
    EXPECT_EQ(network.upgrade.url, "wss://api.cloudflare.com/client/v4/accounts/" + std::string(32, 'a') + "/browser-run/devtools/browser?keep_alive=60000");
    EXPECT_EQ(network.upgrade.headers[0].second, "Bearer fixture-token");
    EXPECT_EQ(cdp->call("Browser.getVersion"), R"({"product":"remote"})");
    cdp = client.connect("remote-id"); EXPECT_TRUE(network.upgrade.url.ends_with("/remote-id"));
    EXPECT_THROW(client.connect("../bad"), Error);
}
TEST(BrowserRun, ContentEnvelopeAndNoRedirects) {
    Network network; br::Client client(network, config());
    network.set_handler([](const HttpRequest& r) {
        EXPECT_TRUE(r.url.ends_with("/browser-run/content"));
        EXPECT_EQ(r.body, R"({"url":"https://example.test/"})");
        EXPECT_EQ(r.headers[0].second, "Bearer fixture-token");
        return HttpResponse{200, {}, R"({"success":true,"result":"<h1>Remote</h1>"})", {}, {}};
    });
    EXPECT_EQ(client.content("https://example.test/"), "<h1>Remote</h1>");
    network.set_handler([](const HttpRequest&) { return HttpResponse{302, {}, "private-data", {}, {}}; });
    EXPECT_THROW(client.content("https://example.test/"), Error);
    EXPECT_EQ(network.requests().size(), 2u);
    EXPECT_THROW(client.content("file:///tmp/example"), Error);
}
TEST(BrowserRun, RejectsCredentialsAndBounds) {
    MemoryNetworkClient network;
    auto c = config(); c.api_token += "\r\nInjected: yes"; EXPECT_THROW(br::Client(network, c), Error);
    c = config(); c.account_id = "../x"; EXPECT_THROW(br::Client(network, c), Error);
    c = config(); c.keep_alive_ms = 1; EXPECT_THROW(br::Client(network, c), Error);
}
TEST(BrowserRun, ConfigParsingAndExplicitSnapshot) {
    const auto project = parse_project_config("[cloudflare]\naccount_id='account'\napi_token='fixture-token'\n[oauth]\nclient_id='client'\nscopes='scope'\n");
    EXPECT_EQ(project.cloudflare_account_id, "account"); EXPECT_EQ(project.cloudflare_api_token, "fixture-token");
    EXPECT_EQ(project.oauth.client_id, "client"); EXPECT_EQ(project.oauth.scopes, "scope");
    Browser normal; auto live = normal.create_session(); EXPECT_THROW(br::import_html(*live, "<p>x</p>", "https://example.test/"), Error);
    BrowserConfig settings; settings.javascript = false; Browser offline(settings); auto session = offline.create_session();
    br::import_html(*session, "<h1>remote</h1><script>throw 1</script>", "https://example.test/");
    EXPECT_EQ(session->document()->query_selector("h1")->text(), "remote");
}
#ifdef __unix__
TEST(BrowserRun, EnvironmentOverridesTomlWithoutNetwork) {
    struct Env {
        std::string name, prior; bool existed;
        Env(const char* key, const char* value) : name(key), existed(std::getenv(key) != nullptr) {
            if (existed) prior = std::getenv(key);
            ::setenv(key, value, 1);
        }
        ~Env() { if (existed) ::setenv(name.c_str(), prior.c_str(), 1); else ::unsetenv(name.c_str()); }
    } account("CLOUDFLARE_ACCOUNT_ID", "env-account"), token("CLOUDFLARE_API_TOKEN", "env-fixture");
    ProjectConfig project; project.cloudflare_account_id = "toml-account"; project.cloudflare_api_token = "toml-fixture";
    MemoryNetworkClient network;
    auto c = br::resolve_config(project, network);
    EXPECT_EQ(c.account_id, "env-account"); EXPECT_EQ(c.api_token, "env-fixture");
    EXPECT_TRUE(network.requests().empty());
}
#endif
