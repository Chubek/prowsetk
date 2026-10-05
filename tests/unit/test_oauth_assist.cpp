#include <gtest/gtest.h>
#include "prowsetk/oauth_assist.hpp"
#include "prowsetk/error.hpp"
#include "prowsetk/project_config.hpp"
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
using namespace prowsetk;
namespace oa = prowsetk::oauth_assist;
namespace {
oa::Config config() { oa::Config c; c.client_id = "test-client"; c.scopes = "test-scope offline_access"; return c; }
std::string state(const oa::Authorization& flow) {
    auto url = flow.authorization_url(); const auto begin = url.find("&state=") + 7; return url.substr(begin, url.find('&', begin) - begin);
}
}
#ifdef PROWSETK_TEST_OPENSSL
TEST(OAuthAssist, PkceExchangeStateAndReplay) {
    auto c = config(); oa::Authorization flow(c);
    EXPECT_NE(flow.authorization_url().find("code_challenge_method=S256"), std::string::npos);
    MemoryNetworkClient network;
    network.set_handler([](const HttpRequest& request) {
        EXPECT_EQ(request.method, "POST"); EXPECT_EQ(request.url, "https://dash.cloudflare.com/oauth2/token");
        EXPECT_NE(request.body.find("code_verifier="), std::string::npos);
        EXPECT_NE(request.body.find("code=code%26value"), std::string::npos);
        HttpResponse r; r.status = 200; r.body = R"({"access_token":"fixture-access","refresh_token":"fixture-refresh","token_type":"Bearer","expires_in":3600})"; return r;
    });
    const auto redirect = c.redirect_uri + "?code=code%26value&state=" + state(flow);
    const auto token = flow.finish(network, redirect);
    EXPECT_FALSE(oa::expired(token)); EXPECT_EQ(token.refresh_token, "fixture-refresh");
    EXPECT_THROW(flow.finish(network, redirect), Error); EXPECT_EQ(network.requests().size(), 1u);
    oa::Authorization bad(c);
    EXPECT_THROW(bad.finish(network, c.redirect_uri + "?code=x&state=wrong"), Error);
    EXPECT_EQ(network.requests().size(), 1u);
}
TEST(OAuthAssist, RejectsRedirectsAndInvalidTokens) {
    for (const auto* body : {R"({"access_token":"x","token_type":"Basic","expires_in":12})", R"({"access_token":"x","token_type":"Bearer","expires_in":0})", R"({"access_token":"x","access_token":"y","token_type":"Bearer","expires_in":12})"}) {
        oa::Authorization flow(config()); MemoryNetworkClient network;
        network.set_response(config().token_endpoint, HttpResponse{200, {}, body, {}, {}});
        EXPECT_THROW(flow.finish(network, config().redirect_uri + "?code=x&state=" + state(flow)), Error);
    }
    oa::Authorization flow(config()); MemoryNetworkClient network;
    network.set_response(config().token_endpoint, HttpResponse{302, {}, "private-error", {}, {}});
    EXPECT_THROW(flow.finish(network, config().redirect_uri + "?code=x&state=" + state(flow)), Error);
    EXPECT_EQ(network.requests().size(), 1u);
}
TEST(OAuthAssist, RejectsDuplicateStateAndUntrustedEndpoints) {
    MemoryNetworkClient network;
    oa::Authorization flow(config());
    const auto s = state(flow);
    EXPECT_THROW(flow.finish(network, config().redirect_uri + "?code=x&state=" + s + "&state=" + s), Error);
    EXPECT_TRUE(network.requests().empty());
    auto c = config(); c.token_endpoint = "http://example.test/token";
    EXPECT_THROW(oa::Authorization{c}, Error);
    c = config(); c.authorization_endpoint = "https://user@example.test/auth";
    EXPECT_THROW(oa::Authorization{c}, Error);
    c = config(); c.redirect_uri = "http://example.test/callback";
    EXPECT_THROW(oa::Authorization{c}, Error);
}
#endif
TEST(OAuthAssist, RefreshRetainsTokenWhenNotRotated) {
    MemoryNetworkClient network;
    network.set_response(config().token_endpoint, HttpResponse{200, {}, R"({"access_token":"new-access","token_type":"Bearer","expires_in":3600})", {}, {}});
    const auto token = oa::refresh(network, config(), {"old-access", "old-refresh", 1});
    EXPECT_EQ(token.refresh_token, "old-refresh"); EXPECT_EQ(token.access_token, "new-access");
    EXPECT_NE(network.requests()[0].body.find("grant_type=refresh_token"), std::string::npos);
}
TEST(OAuthAssist, CachePermissionsIdentityAndSymlinkRejection) {
    auto directory = std::filesystem::canonical(TEST_BINARY_DIR) / "oauth-cache-unit";
    std::filesystem::remove_all(directory);
    oa::save(directory, config(), {"fixture-access", "fixture-refresh", 2000000000});
    struct stat mode{}; ASSERT_EQ(::stat((directory / "token.json").c_str(), &mode), 0);
    EXPECT_EQ(mode.st_mode & 0777, 0600);
    EXPECT_EQ(oa::load(directory, config()).access_token, "fixture-access");
    EXPECT_THROW(oa::save(directory, config(), {"", "", 0}), Error);
    EXPECT_EQ(oa::load(directory, config()).access_token, "fixture-access");
    auto other = config(); other.client_id = "other"; EXPECT_THROW(oa::load(directory, other), Error);
    ::chmod((directory / "token.json").c_str(), 0644); EXPECT_THROW(oa::load(directory, config()), Error);
    ::chmod((directory / "token.json").c_str(), 0600); oa::logout(directory, config());
    std::filesystem::create_symlink(directory / "missing", directory / "token.json");
    EXPECT_THROW(oa::load(directory, config()), Error);
    std::filesystem::remove_all(directory);
}
