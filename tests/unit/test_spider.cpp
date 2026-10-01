#include <gtest/gtest.h>
#include <prowsetk/plugins/spider.hpp>
#include <prowsetk/lua_runtime.hpp>
#include <prowsetk/plugin_registry.hpp>
#include <filesystem>
#include <sys/stat.h>

namespace spider = prowsetk::plugins::spider;
namespace {
class SpiderTest : public ::testing::Test {
protected:
    std::filesystem::path directory;
    void SetUp() override {
        directory = std::filesystem::path(TEST_BINARY_DIR) / ("spider-" + std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
        std::filesystem::remove_all(directory);
    }
    void TearDown() override { std::filesystem::remove_all(directory); }
    std::unique_ptr<prowsetk::MemoryNetworkClient> network() {
        auto client = std::make_unique<prowsetk::MemoryNetworkClient>();
        client->set_handler([](const prowsetk::HttpRequest& request) {
            prowsetk::HttpResponse response;
            response.status = request.url.ends_with("robots.txt") ? 404 : 200;
            response.final_url = request.url;
            response.body = "<title>Test</title><a href='/child'>child</a><a href='/child#again'>duplicate</a><a href='https://other.test/'>outside</a>";
            return response;
        });
        return client;
    }
};
TEST_F(SpiderTest, CacheDurablePrefixPaginationAndDelete) {
    {
        spider::Cache cache(directory);
        cache.write({{"record/a", "one"}, {"record/b", "two"}, {"state", "private"}});
        EXPECT_EQ(cache.query("record/", 1).at(0).first, "record/a");
        EXPECT_EQ(cache.query("record/", 1, "record/a").at(0).second, "two");
        EXPECT_TRUE(cache.query("missing").empty());
    }
    spider::Cache reopened(directory);
    EXPECT_EQ(reopened.get("record/a"), "one");
    reopened.write({{"record/c", "three"}}, {"record/a"});
    EXPECT_FALSE(reopened.get("record/a"));
    EXPECT_EQ(reopened.query("record/").size(), 2u);
    EXPECT_THROW(reopened.query("", 1001), std::exception);
}
TEST_F(SpiderTest, CacheTransactionAbortsAndPermissionsArePrivate) {
    spider::Cache cache(directory, 1024u * 1024u);
    cache.write({{"record/a", "original"}});
    EXPECT_THROW(cache.write({{"record/a", "changed"}, {"too-large", std::string(2u * 1024u * 1024u, 'x')}}), std::exception);
    EXPECT_EQ(cache.get("record/a"), "original");
    struct stat info{};
    ASSERT_EQ(::stat((directory / "data.mdb").c_str(), &info), 0);
    EXPECT_EQ(info.st_mode & 0077, 0u);
}
TEST_F(SpiderTest, CacheRejectsUnsafeDirectory) {
    std::filesystem::create_directories(directory);
    ASSERT_EQ(::chmod(directory.c_str(), 0755), 0);
    EXPECT_THROW(spider::Cache cache(directory), std::exception);
}
TEST_F(SpiderTest, FramingPreservesArbitraryStringsAndRejectsMalformedLengths) {
    const std::vector<std::string> values{"", "hello\nworld", std::string("a\0b", 3), "é"};
    EXPECT_EQ(spider::unpack(spider::pack(values)), values);
    EXPECT_THROW(spider::unpack("3:ab"), std::exception);
    EXPECT_THROW(spider::unpack("-1:a"), std::exception);
    EXPECT_FALSE(spider::valid_name("../oops"));
    EXPECT_TRUE(spider::valid_name("booking.com"));
}
TEST_F(SpiderTest, CrawlDeduplicatesAndRespectsDepthAndOrigin) {
    auto client = network(); auto* observed = client.get();
    spider::Engine engine("test", directory, std::move(client));
    ASSERT_TRUE(engine.command({"crawl", "https://example.test/", "10", "1", "0"}).ok);
    ASSERT_TRUE(engine.tick()); ASSERT_TRUE(engine.tick());
    EXPECT_FALSE(engine.tick());
    EXPECT_EQ(observed->requests().size(), 3u); // robots, root, child
    EXPECT_NE(engine.command({"status"}).json.find("\"pages\":2"), std::string::npos);
    EXPECT_FALSE(engine.command({"enqueue", "https://other.test/"}).ok);
    EXPECT_FALSE(engine.command({"navigate", "file:///etc/passwd"}).ok);
}
TEST_F(SpiderTest, FrontierSurvivesRestartAndRecoveryIsPaused) {
    {
        spider::Engine engine("test", directory, network());
        ASSERT_TRUE(engine.command({"crawl", "https://example.test/", "10", "2", "0"}).ok);
        ASSERT_TRUE(engine.tick());
    }
    spider::Engine recovered("test", directory, network());
    EXPECT_FALSE(recovered.running()); EXPECT_FALSE(recovered.tick());
    EXPECT_NE(recovered.command({"status"}).json.find("\"pending\":1"), std::string::npos);
    ASSERT_TRUE(recovered.command({"resume"}).ok);
    EXPECT_TRUE(recovered.tick());
    EXPECT_NE(recovered.command({"status"}).json.find("\"pages\":2"), std::string::npos);
}
TEST_F(SpiderTest, RobotsLongestMatchAndExplicitAgent) {
    auto client = network(); auto* observed = client.get();
    client->set_handler([](const prowsetk::HttpRequest& request) {
        prowsetk::HttpResponse response; response.status = 200; response.final_url = request.url;
        response.body = request.url.ends_with("robots.txt") ?
            "User-agent: *\nDisallow: /\nUser-agent: ProwseTkSpider\nDisallow: /blocked*\nAllow: /blocked/allowed$\n" :
            "<a href='/blocked'>no</a><a href='/blocked/allowed'>yes</a>";
        return response;
    });
    spider::Engine engine("test", directory, std::move(client));
    ASSERT_TRUE(engine.command({"crawl", "https://example.test/", "10", "1", "0"}).ok);
    ASSERT_TRUE(engine.tick()); ASSERT_TRUE(engine.tick()); ASSERT_TRUE(engine.tick());
    EXPECT_EQ(observed->requests().size(), 3u);
    EXPECT_EQ(observed->requests().back().url, "https://example.test/blocked/allowed");
}
TEST_F(SpiderTest, RobotsWildcardOnlyDisallowAndAllowTie) {
    auto client = network(); auto* observed = client.get();
    client->set_handler([](const prowsetk::HttpRequest& request) {
        prowsetk::HttpResponse response; response.status = 200; response.final_url = request.url;
        response.body = request.url.ends_with("robots.txt") ?
            "User-agent: *\nDisallow: *\n" :
            "<h1>Allowed</h1>";
        return response;
    });
    {
        spider::Engine engine("test", directory, std::move(client));
        ASSERT_TRUE(engine.command({"crawl", "https://example.test/", "1", "0", "0"}).ok);
        ASSERT_TRUE(engine.tick());
        EXPECT_EQ(observed->requests().size(), 1u); // Only robots; wildcard denies the page.
        EXPECT_NE(engine.command({"status"}).json.find("\"pages\":0"), std::string::npos);
    }
    auto allowed_client = network(); auto* allowed_observed = allowed_client.get();
    allowed_client->set_handler([](const prowsetk::HttpRequest& request) {
        prowsetk::HttpResponse response; response.status = 200; response.final_url = request.url;
        response.body = request.url.ends_with("robots.txt") ?
            "User-agent: *\nAllow: *\nDisallow: *\n" : "<h1>Allowed</h1>";
        return response;
    });
    spider::Engine recovered("test", directory, std::move(allowed_client));
    ASSERT_TRUE(recovered.command({"crawl", "https://example.test/", "1", "0", "0"}).ok);
    ASSERT_TRUE(recovered.tick());
    EXPECT_EQ(allowed_observed->requests().size(), 2u);
    EXPECT_NE(recovered.command({"status"}).json.find("\"pages\":1"), std::string::npos);
}
TEST_F(SpiderTest, PageLimitPauseAndQueryRedaction) {
    spider::Engine engine("test", directory, network());
    ASSERT_TRUE(engine.command({"create", "https://example.test/", "1", "0", "0"}).ok);
    ASSERT_TRUE(engine.command({"load-html", "<input value='hidden-value'><textarea>private-form</textarea><script>secret-code</script><a href='/?token=private-token'>Link</a>"}).ok);
    const auto result = engine.command({"cache", "get", "page/000"});
    ASSERT_TRUE(result.ok);
    EXPECT_EQ(result.json.find("hidden-value"), std::string::npos);
    EXPECT_EQ(result.json.find("private-form"), std::string::npos);
    EXPECT_EQ(result.json.find("secret-code"), std::string::npos);
    EXPECT_EQ(result.json.find("private-token"), std::string::npos);
    ASSERT_TRUE(engine.command({"cache", "put", "access_token", "sensitive-value"}).ok);
    EXPECT_EQ(engine.command({"cache", "get", "record/access_token"}).json, "\"[REDACTED]\"");
    EXPECT_FALSE(engine.command({"cache", "get", "state"}).ok);
    ASSERT_TRUE(engine.command({"resume"}).ok); EXPECT_TRUE(engine.tick()); EXPECT_FALSE(engine.tick());
    EXPECT_FALSE(engine.running());
}
TEST_F(SpiderTest, DriverUsesExistingSessionAndStagesWrites) {
    if (!prowsetk::LuaRuntime::available()) GTEST_SKIP();
    spider::Engine engine("test", directory, network());
    ASSERT_TRUE(engine.command({"create", "https://example.test/"}).ok);
    const std::string driver = R"LUA(
        local spider = require('lspider')
        function main(args)
            spider.load_html(args.html, 'https://example.test/')
            assert(spider.document():title() == 'Driver')
            spider.cache.put('result', 'ready')
            assert(spider.cache.get('result') == 'ready')
            spider.enqueue('https://example.test/next', 1)
            return 0
        end
    )LUA";
    ASSERT_TRUE(engine.command({"driver", driver, "html", "<title>Driver</title>"}).ok);
    EXPECT_EQ(engine.command({"cache", "get", "record/result"}).json, "\"ready\"");
    EXPECT_NE(engine.command({"status"}).json.find("\"pending\":2"), std::string::npos);
    const std::string failed = "local s=require('lspider'); function main(args) s.cache.put('bad','value'); s.enqueue('https://other.test/'); return 0 end";
    EXPECT_FALSE(engine.command({"driver", failed}).ok);
    EXPECT_FALSE(engine.command({"cache", "get", "record/bad"}).ok);
    EXPECT_FALSE(engine.command({"driver", "function main(args) while true do end end"}).ok);
}
TEST_F(SpiderTest, NativePluginLoadsAndKeepsAbi) {
    prowsetk::Browser browser;
    const auto& descriptor = browser.plugins().load_native(SPIDER_PLUGIN_PATH);
    EXPECT_EQ(descriptor.abi_version, "2");
    EXPECT_EQ(browser.plugins().initialize_all(), 1u);
    EXPECT_THROW(browser.plugins().configure_all({{"unknown", "value"}}), std::exception);
    ASSERT_EQ(browser.plugins().configure_all({{"spider.cache_directory", directory.string()}}), 1u);
    auto session = browser.create_session();
    session->load_html("<title>Example</title>", "https://example.test/?token=private-value");
    browser.plugins().shutdown_all();
    spider::Cache cache(directory);
    const auto url = cache.get("record/latest-url");
    ASSERT_TRUE(url);
    EXPECT_EQ(url->find("private-value"), std::string::npos);
    EXPECT_NE(url->find("REDACTED"), std::string::npos);
}
TEST_F(SpiderTest, OriginPolicyCoversRedirectsAndPageScripts) {
    auto client = network(); auto* observed = client.get();
    client->set_handler([](const prowsetk::HttpRequest& request) {
        prowsetk::HttpResponse response; response.status = 302; response.final_url = request.url;
        response.headers.emplace_back("Location", "https://other.test/next");
        return response;
    });
    spider::Engine engine("test", directory, std::move(client));
    ASSERT_TRUE(engine.command({"create", "https://example.test/"}).ok);
    EXPECT_FALSE(engine.command({"navigate", "https://example.test/redirect"}).ok);
    EXPECT_EQ(observed->requests().size(), 1u);
    ASSERT_TRUE(engine.command({"load-html", "<script>fetch('https://other.test/api').catch(function() {});</script><h1>Test</h1>"}).ok);
    EXPECT_EQ(observed->requests().size(), 1u);
    EXPECT_FALSE(engine.command({"enqueue", "https://user:pass@example.test/"}).ok);
}
TEST_F(SpiderTest, RobotsFailuresRetryWithoutFetchingAndBudgetIsBounded) {
    auto client = network(); auto* observed = client.get();
    client->set_handler([](const prowsetk::HttpRequest& request) {
        prowsetk::HttpResponse response; response.status = 503; response.final_url = request.url;
        return response;
    });
    spider::Engine engine("test", directory, std::move(client));
    ASSERT_TRUE(engine.command({"crawl", "https://example.test/", "10", "1", "0"}).ok);
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(engine.command({"resume"}).ok);
        EXPECT_TRUE(engine.tick());
    }
    EXPECT_FALSE(engine.tick());
    ASSERT_EQ(observed->requests().size(), 4u);
    for (const auto& request : observed->requests()) EXPECT_TRUE(request.url.ends_with("robots.txt"));
    EXPECT_NE(engine.command({"status"}).json.find("\"pending\":0"), std::string::npos);
}
TEST_F(SpiderTest, RevisitSchedulePersistsAndCacheIsQueryable) {
    {
        spider::Engine engine("test", directory, network());
        ASSERT_TRUE(engine.command({"watch", "https://example.test/", "3600", "1", "0", "0"}).ok);
        EXPECT_TRUE(engine.tick()); EXPECT_FALSE(engine.tick());
        const auto status = engine.command({"status"}).json;
        EXPECT_NE(status.find("\"watch_seconds\":3600"), std::string::npos);
        const auto query = engine.command({"cache", "select", "a"});
        ASSERT_TRUE(query.ok); EXPECT_NE(query.json.find("child"), std::string::npos);
    }
    spider::Engine engine("test", directory, network());
    EXPECT_FALSE(engine.running());
    EXPECT_NE(engine.command({"status"}).json.find("\"watch_seconds\":3600"), std::string::npos);
    ASSERT_TRUE(engine.command({"resume"}).ok); EXPECT_FALSE(engine.tick());
    ASSERT_TRUE(engine.command({"unwatch"}).ok);
    EXPECT_NE(engine.command({"status"}).json.find("\"watch_seconds\":0"), std::string::npos);
}
}
