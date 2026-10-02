#include <gtest/gtest.h>
#include "support.hpp"
#include "lua_sources.hpp"
#include <cstdlib>

namespace pa = prowsetk::automation;
namespace {
prowsetk::BrowserConfig no_javascript() {
    prowsetk::BrowserConfig config; config.javascript = false; return config;
}
prowsetk::HttpResponse response(std::string body = {}, int status = 200,
                               std::vector<std::pair<std::string, std::string>> headers = {}) {
    prowsetk::HttpResponse result;
    result.status = status; result.body = std::move(body); result.headers = std::move(headers); return result;
}
class Crawl : public testing::Test {
protected:
    prowsetk::Browser browser{no_javascript()};
    prowsetk::MemoryNetworkClient* network = nullptr;
    std::shared_ptr<prowsetk::Session> session;
    prowsetk::LuaRuntime lua;
    void SetUp() override {
        auto memory = std::make_unique<prowsetk::MemoryNetworkClient>(); network = memory.get();
        browser.set_network_client(std::make_unique<pa::RestrictedNetwork>(std::vector<std::string>{"https://example.test"}, 100, std::move(memory)));
        network->set_response("https://example.test/robots.txt", response("User-agent: *\nDisallow: /private\n"));
        session = browser.create_session(); lua.bind_browser(&browser); lua.bind_session(session);
        pa::load_module(lua, "ezlogin", pa::ezlogin_source);
        pa::load_module(lua, "scrape_endpoints", pa::scrape_endpoints_source);
        pa::load_module(lua, "lcrawler", pa::lcrawler_source);
        ASSERT_TRUE(lua.run("function main(args) return require('lcrawler').run(session,args) end").ok);
    }
    std::string run(std::vector<prowsetk::LuaArgument> args = {}) {
        args.push_back({"url", "url", "https://example.test/"});
        return pa::call(lua, "main", args);
    }
    std::string output(const char* kind) { return pa::call(lua, "__crawler_result", {{"kind", "string", kind}}); }
};
}
TEST_F(Crawl, BreadthFirstDedupRobotsRedirectPolicyAndMergedScraping) {
    network->set_response("https://example.test/", response("<h1>Root</h1><a href='/a#one'>A</a><a href='/a#two'>dup</a><a href='/b'>B</a><a href='/private'>private</a><a href='/logout'>logout</a><a href='https://evil.test/'>outside</a><script>fetch('/api/root')</script>"));
    network->set_response("https://example.test/a", response("<h2>A</h2><a href='/c'>C</a><a href='/escape'>escape</a><script>fetch('/api/a')</script>"));
    network->set_response("https://example.test/b", response("<h2>B</h2>"));
    network->set_response("https://example.test/c", response("<h2>C</h2>"));
    network->set_response("https://example.test/escape", response({}, 302, {{"Location", "https://evil.test/leak"}}));
    ASSERT_EQ(run(), "0");
    const auto pages = output("pages");
    EXPECT_LT(pages.find("Root"), pages.find("\"text\":\"A\""));
    EXPECT_LT(pages.find("\"text\":\"A\""), pages.find("\"text\":\"B\""));
    EXPECT_LT(pages.find("\"text\":\"B\""), pages.find("\"text\":\"C\""));
    EXPECT_NE(output("openapi").find("/api/a"), std::string::npos);
    EXPECT_NE(output("postman").find("/api/root"), std::string::npos);
    for (const auto& request : network->requests()) {
        EXPECT_EQ(request.url.find("evil.test"), std::string::npos);
        EXPECT_EQ(request.url.find("/private"), std::string::npos);
        EXPECT_EQ(request.url.find("/logout"), std::string::npos);
    }
    EXPECT_NE(output("summary").find("\"failures\":1"), std::string::npos);
}
TEST_F(Crawl, BoundedFrontierReportsIncompleteCoverage) {
    network->set_response("https://example.test/", response("<h1>Root</h1><a href='/a'>A</a><a href='/b'>B</a>"));
    ASSERT_EQ(run({{"max_pages", "integer", "1"}, {"max_frontier", "integer", "2"}}), "0");
    EXPECT_NE(output("summary").find("\"truncated\":true"), std::string::npos);
    EXPECT_NE(output("summary").find("\"complete\":false"), std::string::npos);
    EXPECT_EQ(network->requests().size(), 2u);
}
TEST_F(Crawl, EzloginFormUsesScopedCookiesAndPositiveDomEvidence) {
    ::setenv("PWTK_TEST_USER", "fixture-user", 1); ::setenv("PWTK_TEST_PASS", "fixture-password", 1);
    bool password_seen = false;
    network->set_handler([&](const prowsetk::HttpRequest& request) {
        if (request.url.ends_with("robots.txt")) return response({}, 404);
        if (request.method == "POST") {
            password_seen = request.body.find("fixture-password") != std::string::npos;
            return response({}, 303, {{"Location", "/"}, {"Set-Cookie", "session=fixture-session; Path=/; Secure"}});
        }
        bool authenticated = false;
        for (const auto& [name, value] : request.headers) if (name == "Cookie" && value.find("fixture-session") != value.npos) authenticated = true;
        return response(authenticated ? "<h1>Admin</h1><a href='/logout'>out</a><input value='private-form'><script>fetch('/api?token=private-query')</script>" : "<form action='/login' method='post'><input name='username'><input name='password' type='password'></form>");
    });
    const auto code = run({{"login_mode", "string", "form"}, {"username_env", "string", "PWTK_TEST_USER"}, {"password_env", "string", "PWTK_TEST_PASS"}, {"success_selector", "string", "a[href='/logout']"}, {"query", "string", "select text, attr('value') from <*>"}});
    ::unsetenv("PWTK_TEST_USER"); ::unsetenv("PWTK_TEST_PASS");
    ASSERT_EQ(code, "0"); EXPECT_TRUE(password_seen);
    for (const auto* kind : {"pages", "openapi", "postman", "summary"}) {
        EXPECT_EQ(output(kind).find("fixture-password"), std::string::npos);
        EXPECT_EQ(output(kind).find("fixture-session"), std::string::npos);
        EXPECT_EQ(output(kind).find("private-"), std::string::npos);
    }
    for (const auto& [name, value] : session->headers()) { (void)value; EXPECT_NE(name, "Cookie"); }
}
TEST_F(Crawl, ScriptLogoutWordsAreNotLoginConfirmation) {
    network->set_response("https://example.test/", response("<script>var words='logout dashboard';</script><h1>Not authenticated</h1>"));
    ASSERT_EQ(run({{"login_mode", "string", "form"}, {"success_selector", "string", "a[href='/logout']"}}), "3");
    EXPECT_TRUE(output("pages").empty());
}
TEST_F(Crawl, OfflineModeDoesNotReadCredentialsOrIssueRequests) {
    ASSERT_EQ(run({{"html", "string", "<h1>Offline</h1><a href='/next'>next</a>"}, {"login_mode", "string", "form"}}), "0");
    EXPECT_TRUE(network->requests().empty());
    EXPECT_NE(output("pages").find("Offline"), std::string::npos);
}

TEST_F(Crawl, SharedScraperExportsRedactAllProvenanceUrls) {
    ASSERT_TRUE(lua.run(R"(
        local scrape = require('scrape_endpoints')
        local endpoints = {{path='/api/items', method='get', confidence=0.7,
            url='https://private-user:private-pass@example.test/api/items?t%6Fken=private-query&page=2',
            source='https://example.test/page?secret=private-source',
            final_url='https://example.test/api/items?auth=private-final#private-fragment'}}
        for _, output in ipairs({scrape.render_openapi_yaml(endpoints), scrape.render_postman_json(endpoints)}) do
            assert(not output:find('private-',1,true))
            assert(output:find('page=2',1,true))
        end
    )").ok) << lua.last_error();
}

TEST_F(Crawl, MergedOperationsHaveOneMethodPerPath) {
    network->set_response("https://example.test/", response("<h1>Root</h1><a href='/next'>next</a><script>fetch('/api/items?page=1')</script>"));
    network->set_response("https://example.test/next", response("<h1>Next</h1><script>fetch('/api/items?page=2&limit=5')</script>"));
    ASSERT_EQ(run(), "0");
    const auto yaml = output("openapi");
    const auto operation = yaml.find("    get:");
    ASSERT_NE(operation, std::string::npos);
    EXPECT_EQ(yaml.find("    get:", operation + 1), std::string::npos);
    EXPECT_NE(yaml.find("name: 'limit'"), std::string::npos);
    EXPECT_NE(yaml.find("name: 'page'"), std::string::npos);
}
