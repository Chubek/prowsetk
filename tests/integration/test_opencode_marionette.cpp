#include <gtest/gtest.h>
#include "prowsetk/plugins/opencode_marionette.hpp"
#include "prowsetk/error.hpp"
#include "prowsetk/cookie_import.hpp"
#include "prowsetk/lua_runtime.hpp"
#include <algorithm>
#include <fstream>
namespace m = prowsetk::plugins::opencode_marionette;
namespace b = prowsetk::plugins::opencode_bridge;
namespace {
prowsetk::HttpResponse response(std::string body, std::string content_type = "application/json", int status = 200) {
    return {status, {{"Content-Type", std::move(content_type)}}, std::move(body), {}, {}};
}
std::string answer(std::string action) {
    return "{\"info\":{\"role\":\"assistant\"},\"parts\":[{\"type\":\"text\",\"text\":" +
        b::json_escape("{\"action\":" + b::json_escape(action) + "}") + "}]}";
}
struct Fixture : testing::Test {
    prowsetk::Browser browser;
    prowsetk::MemoryNetworkClient* page = nullptr;
    prowsetk::MemoryNetworkClient agent;
    std::shared_ptr<prowsetk::Session> session;
    void SetUp() override {
        auto transport = std::make_unique<prowsetk::MemoryNetworkClient>();
        page = transport.get();
        page->set_handler([](const auto&) { return response(R"({"id":123,"active":true,"token":"response-private"})"); });
        browser.set_network_client(std::move(transport));
        session = browser.create_session();
        session->set_header("Authorization", "Bearer page-private");
        session->load_html("<a href='/api/first'>first</a>", "https://page.test/start?token=url-private");
        agent.set_handler([](const auto& request) {
            if (request.url.ends_with("/session")) return response(R"({"id":"agent-session"})");
            return response(answer("stop"));
        });
    }
    b::BridgeConfig config() {
        b::BridgeConfig config;
        config.username = "opencode";
        config.password = "agent-private";
        config.max_requests = 65;
        return config;
    }
    m::Decisions policy() { return m::parse_decisions(R"({"version":1,"goal":"discover","actions":[]})"); }
};
TEST_F(Fixture, StopCollectsTypedSchemaAndIsolatesCredentials) {
    b::OpenCodeClient client(agent, config());
    auto result = m::run(*session, client, policy());
    EXPECT_TRUE(result.stopped);
    EXPECT_FALSE(result.coverage_complete);
    EXPECT_EQ(result.steps, 0u);
    ASSERT_EQ(result.extraction.schemas.size(), 1u);
    EXPECT_EQ(result.extraction.probe_count, 1u);
    EXPECT_NE(result.extraction.openapi_yaml.find("integer"), std::string::npos);
    EXPECT_NE(result.extraction.openapi_yaml.find("boolean"), std::string::npos);
    EXPECT_NE(result.extraction.openapi_yaml.find("coverage-complete: false"), std::string::npos);
    EXPECT_EQ(result.extraction.openapi_yaml.find("response-private"), std::string::npos);
    EXPECT_EQ(result.extraction.postman_json.find("response-private"), std::string::npos);
    for (const auto& request : agent.requests()) {
        EXPECT_EQ(request.body.find("url-private"), std::string::npos);
        EXPECT_EQ(request.body.find("page-private"), std::string::npos);
        for (const auto& h : request.headers) EXPECT_EQ(h.second.find("page-private"), std::string::npos);
    }
    for (const auto& request : page->requests()) {
        for (const auto& h : request.headers) EXPECT_EQ(h.second.find("agent-private"), std::string::npos);
    }
    EXPECT_EQ(browser.events().handler_count(prowsetk::EventType::BeforeRequest), 0u);
}
TEST_F(Fixture, ClickDrainsFetchAndNavigationAccumulatesAcrossPages) {
    if (prowsetk::make_javascript_runtime()->name() == "null") GTEST_SKIP();
    page->set_handler([](const auto& request) {
        if (request.url == "https://page.test/next") return response("<a href='/api/second'>second</a>", "text/html");
        return response(R"({"count":3})");
    });
    session->load_html(R"html(<a href='/api/first'>first</a><button id='load'>load</button>
        <a id='next' href='/next'>next</a><script>
        document.getElementById('load').addEventListener('click',()=>Promise.resolve().then(()=>fetch('/api/created',{method:'POST',body:'{}'})));
        </script>)html", "https://page.test/start");
    unsigned turn = 0;
    agent.set_handler([&](const auto& request) {
        if (request.url.ends_with("/session")) return response(R"({"id":"agent-session"})");
        const std::string choices[] = {"load", "next", "stop"};
        return response(answer(choices[turn++]));
    });
    auto decisions = m::parse_decisions(R"({"version":1,"goal":"discover","actions":[{"id":"load","kind":"click","selector":"#load"},{"id":"next","kind":"click","selector":"#next"}]})");
    b::OpenCodeClient client(agent, config());
    auto result = m::run(*session, client, decisions);
    EXPECT_EQ(result.steps, 2u);
    EXPECT_EQ(session->current_url(), "https://page.test/next");
    ASSERT_GE(result.extraction.schemas.size(), 3u);
    EXPECT_NE(result.extraction.openapi_yaml.find("/api/first"), std::string::npos);
    EXPECT_NE(result.extraction.openapi_yaml.find("/api/second"), std::string::npos);
    EXPECT_NE(result.extraction.openapi_yaml.find("/api/created"), std::string::npos);
    EXPECT_EQ(std::count_if(page->requests().begin(), page->requests().end(), [](const auto& request) {
        return request.method == "POST";
    }), 1);
}
TEST_F(Fixture, TrustedTypingValueIsNeverTransmittedToAgent) {
    if (prowsetk::make_javascript_runtime()->name() == "null") GTEST_SKIP();
    session->load_html("<input id='input'><textarea>form-private</textarea><script>var x='script-private'</script>", "https://page.test/");
    unsigned turn = 0;
    agent.set_handler([&](const auto& request) {
        EXPECT_EQ(request.body.find("typed-private"), std::string::npos);
        EXPECT_EQ(request.body.find("form-private"), std::string::npos);
        EXPECT_EQ(request.body.find("script-private"), std::string::npos);
        if (request.url.ends_with("/session")) return response(R"({"id":"agent-session"})");
        return response(answer(turn++ ? "stop" : "input"));
    });
    auto decisions = m::parse_decisions(R"({"version":1,"goal":"discover","actions":[{"id":"input","kind":"type","selector":"#input","value":"typed-private"}]})");
    b::OpenCodeClient client(agent, config());
    EXPECT_EQ(m::run(*session, client, decisions).steps, 1u);
    EXPECT_EQ(session->document()->query_selector("#input")->value(), "typed-private");
}
TEST_F(Fixture, RejectsCrossOriginPolicyBeforeAgentCall) {
    auto decisions = m::parse_decisions(R"({"version":1,"goal":"discover","actions":[{"id":"leave","kind":"navigate","url":"https://other.test/"}]})");
    b::OpenCodeClient client(agent, config());
    EXPECT_THROW(m::run(*session, client, decisions), prowsetk::Error);
    EXPECT_TRUE(agent.requests().empty());
}
TEST_F(Fixture, CrossOriginRedirectNeverReachesTransportAndHooksAreRemoved) {
    page->set_handler([](const auto&) { return prowsetk::HttpResponse{302, {{"Location","https://other.test/private"}}, "", {}, {}}; });
    b::OpenCodeClient client(agent, config());
    EXPECT_THROW(m::run(*session, client, policy()), prowsetk::Error);
    EXPECT_EQ(page->requests().size(), 1u);
    EXPECT_TRUE(agent.requests().empty());
    EXPECT_EQ(browser.events().handler_count(prowsetk::EventType::BeforeRequest), 0u);
    EXPECT_EQ(browser.events().handler_count(prowsetk::EventType::BeforeRedirect), 0u);
}
TEST_F(Fixture, RequestAndProbeBudgetsAreShared) {
    session->load_html("<a href='/api/a'>a</a><a href='/api/b'>b</a>", "https://page.test/");
    auto decisions = policy();
    decisions.max_get_probes = 1;
    b::OpenCodeClient client(agent, config());
    auto result = m::run(*session, client, decisions);
    EXPECT_EQ(result.extraction.probe_count, 1u);
    EXPECT_EQ(page->requests().size(), 1u);
    decisions.max_get_probes = 2;
    decisions.max_page_requests = 1;
    b::OpenCodeClient second(agent, config());
    EXPECT_THROW(m::run(*session, second, decisions), prowsetk::Error);
}
TEST_F(Fixture, UnknownChoiceFailsWithValueFreeErrorAndCleansHooks) {
    agent.set_handler([](const auto& request) {
        if (request.url.ends_with("/session")) return response(R"({"id":"agent-session"})");
        return response(answer("private-invented-action"));
    });
    b::OpenCodeClient client(agent, config());
    try { m::run(*session, client, policy()); FAIL(); }
    catch (const prowsetk::Error& error) {
        EXPECT_EQ(error.code(), prowsetk::ErrorCode::SecurityViolation);
        EXPECT_EQ(std::string(error.what()).find("private"), std::string::npos);
        EXPECT_NE(std::string(error.what()).find("OpenCode decision failed"), std::string::npos);
    }
    EXPECT_EQ(browser.events().handler_count(prowsetk::EventType::AfterResponse), 0u);
}
TEST_F(Fixture, NavigationStepLimitReturnsExplicitPartialCoverage) {
    page->set_handler([](const auto&) { return response("<a href='/api/b'>b</a>", "text/html"); });
    agent.set_handler([](const auto& request) {
        if (request.url.ends_with("/session")) return response(R"({"id":"agent-session"})");
        return response(answer("next"));
    });
    auto decisions = m::parse_decisions(R"({"version":1,"goal":"discover","max_steps":1,"max_get_probes":0,"actions":[{"id":"next","kind":"navigate","url":"/next"}]})");
    b::OpenCodeClient client(agent, config());
    auto result = m::run(*session, client, decisions);
    EXPECT_FALSE(result.stopped);
    EXPECT_EQ(result.reason, "step-limit");
    EXPECT_EQ(result.steps, 1u);
    EXPECT_GE(result.extraction.schemas.size(), 2u);
    EXPECT_NE(result.extraction.openapi_yaml.find("/api/first"), std::string::npos);
    EXPECT_NE(result.extraction.openapi_yaml.find("/api/b"), std::string::npos);
}
TEST_F(Fixture, ExhaustedActionIsRejected) {
    page->set_handler([](const auto&) { return response("<p>next</p>", "text/html"); });
    agent.set_handler([](const auto& request) {
        if (request.url.ends_with("/session")) return response(R"({"id":"agent-session"})");
        return response(answer("next"));
    });
    auto decisions = m::parse_decisions(R"({"version":1,"goal":"discover","max_get_probes":0,"actions":[{"id":"next","kind":"navigate","url":"/next"}]})");
    b::OpenCodeClient client(agent, config());
    EXPECT_THROW(m::run(*session, client, decisions), prowsetk::Error);
    EXPECT_EQ(page->requests().size(), 1u);
}
}

TEST(MarionetteNative, LoadingAndInitializingIsNetworkFree) {
    prowsetk::Browser browser;
    auto transport = std::make_unique<prowsetk::MemoryNetworkClient>();
    auto* network = transport.get();
    browser.set_network_client(std::move(transport));
    const auto& plugin = browser.plugins().load_native(MARIONETTE_PLUGIN_PATH);
    EXPECT_EQ(plugin.name, "opencode-marionette");
    EXPECT_EQ(browser.plugins().initialize_all(), 1u);
    auto session = browser.create_session();
    session->load_html("<p>loaded</p>", "https://page.test/");
    browser.plugins().shutdown_all();
    EXPECT_TRUE(network->requests().empty());
}

TEST_F(Fixture, CrossOriginScriptFetchIsDeniedAfterClick) {
    if (prowsetk::make_javascript_runtime()->name() == "null") GTEST_SKIP();
    session->load_html(R"html(<button id='load'>load</button><script>
        document.getElementById('load').addEventListener('click',()=>fetch('https://other.test/private'));
        </script>)html", "https://page.test/");
    agent.set_handler([](const auto& request) {
        if (request.url.ends_with("/session")) return response(R"({"id":"agent-session"})");
        return response(answer("load"));
    });
    const auto decisions = m::parse_decisions(R"({"version":1,"goal":"discover","max_get_probes":0,"actions":[{"id":"load","kind":"click","selector":"#load"}]})");
    b::OpenCodeClient client(agent, config());
    EXPECT_THROW(m::run(*session, client, decisions), prowsetk::Error);
    EXPECT_TRUE(page->requests().empty());
    EXPECT_EQ(browser.events().handler_count(prowsetk::EventType::BeforeRequest), 0u);
}

TEST_F(Fixture, ProbeUsesOriginalQueryWhileAllReturnedUrlsAreRedacted) {
    session->load_html("<a href='/api/query?token=probe-private'>query</a>", "https://page.test/");
    b::OpenCodeClient client(agent, config());
    const auto result = m::run(*session, client, policy());
    ASSERT_EQ(page->requests().size(), 1u);
    EXPECT_EQ(page->requests()[0].url, "https://page.test/api/query?token=probe-private");
    EXPECT_EQ(result.extraction.openapi_yaml.find("probe-private"), std::string::npos);
    EXPECT_EQ(result.extraction.postman_json.find("probe-private"), std::string::npos);
    for (const auto& endpoint : result.extraction.endpoints) EXPECT_EQ(endpoint.url.find("probe-private"), std::string::npos);
}

TEST_F(Fixture, LogoutDiscoveryDoesNotInvalidateTheAuthenticatedSession) {
    bool logged_in = true;
    page->set_handler([&](const auto& request) {
        if (request.url == "https://page.test/logout") logged_in = false;
        return response(logged_in ? R"({"count":3})" : "{}", "application/json", logged_in ? 200 : 401);
    });
    session->load_html("<a href='/logout'>Log out</a><a id='view' href='/next'>Next</a>", "https://page.test/");
    unsigned turn = 0;
    agent.set_handler([&](const auto& request) {
        if (request.url.ends_with("/session")) return response(R"({"id":"agent-session"})");
        return response(answer(turn++ ? "stop" : "view"));
    });
    b::OpenCodeClient client(agent, config());
    const auto result = m::run(*session, client,
        m::parse_decisions(R"({"version":1,"goal":"view","actions":[{"id":"view","kind":"click","selector":"#view"}]})"));
    EXPECT_TRUE(logged_in);
    EXPECT_EQ(result.steps, 1u);
    EXPECT_EQ(session->current_url(), "https://page.test/next");
    for (const auto& request : page->requests()) EXPECT_NE(request.url, "https://page.test/logout");
    EXPECT_NE(result.extraction.openapi_yaml.find("/logout"), std::string::npos);
}

TEST_F(Fixture, V2PollingControlsClicksAndAccumulatesSchemas) {
    if (prowsetk::make_javascript_runtime()->name() == "null") GTEST_SKIP();
    session->load_html("<button id='view'>view</button><script>document.getElementById('view').onclick="
        "()=>fetch('/api/' + ['details'].join(''));</script>", "https://page.test/");
    std::string messages = R"({"data":[]})";
    unsigned turn = 0;
    agent.set_handler([&](const auto& request) {
        if (request.url.ends_with("/api/session")) {
            EXPECT_NE(request.body.find("\"effect\":\"deny\""), std::string::npos);
            return response(R"({"data":{"id":"ses_decision"}})");
        }
        if (request.method == "GET") {
            EXPECT_TRUE(request.url.ends_with("/message?order=desc&limit=128"));
            return response(messages);
        }
        EXPECT_TRUE(request.url.ends_with("/prompt"));
        EXPECT_NE(request.body.find("\"text\":"), std::string::npos);
        EXPECT_EQ(request.body.find("\"parts\":"), std::string::npos);
        const auto choice = turn++ ? "stop" : "view";
        messages = "{\"data\":[{\"id\":\"msg_" + std::to_string(turn) +
            "\",\"type\":\"assistant\",\"time\":{\"completed\":1},\"content\":[{\"type\":\"text\",\"text\":" +
            b::json_escape("{\"action\":" + b::json_escape(choice) + "}") + "}]}]}";
        return response("{}");
    });
    auto bridge_config = config();
    bridge_config.api_prefix = "/api";
    b::OpenCodeClient client(agent, bridge_config);
    const auto result = m::run(*session, client,
        m::parse_decisions(R"({"version":1,"goal":"view details","actions":[{"id":"view","kind":"click","selector":"#view"}]})"));
    EXPECT_EQ(result.steps, 1u);
    EXPECT_TRUE(result.stopped);
    EXPECT_NE(result.extraction.openapi_yaml.find("/api/details"), std::string::npos);
    EXPECT_NE(result.extraction.openapi_yaml.find("used: true"), std::string::npos);
    EXPECT_NE(result.extraction.openapi_yaml.find("integer"), std::string::npos);
}

TEST(MarionetteBooking, PreparesTheManagedSessionWithCookiesAndPositiveDomEvidence) {
    if (!prowsetk::LuaRuntime::available()) GTEST_SKIP();
    const auto dotenv = std::filesystem::current_path() / "marionette-booking.env";
    { std::ofstream file(dotenv); file << "# imported cookies need no credentials\n"; }
    for (const auto& html : {"<h1>Joe Litty Rooms</h1><a href='/api/rooms'>Rooms</a>",
                            "<script>var message='Joe Litty Rooms logout';</script>",
                            "<h1>Joe Litty Rooms</h1>"}) {
        const bool valid = std::string_view(html).find("/api/rooms") != std::string_view::npos;
        prowsetk::BrowserConfig config;
        config.follow_redirects = false;
        prowsetk::Browser browser(config);
        auto page = std::make_unique<prowsetk::MemoryNetworkClient>();
        auto* transport = page.get();
        page->set_handler([&](const auto& request) {
            for (const auto& header : request.headers) {
                if (header.first == "Cookie") EXPECT_NE(header.second.find("booking-private"), std::string::npos);
            }
            if (request.url.ends_with("/start")) return prowsetk::HttpResponse{302, {{"Location", "/dashboard"}}, "", {}, {}};
            if (request.url.ends_with("/api/rooms")) return response(R"({"rooms":2})");
            return response(html, "text/html", valid ? 200 : 401);
        });
        browser.set_network_client(std::move(page));
        prowsetk::import_cookies_json(browser.storage().cookies(),
            R"([{"domain":"admin.booking.com","path":"/","name":"session","value":"booking-private","secure":true}])");
        const auto session = browser.create_session();
        prowsetk::LuaRuntime lua;
        lua.bind_browser(&browser);
        lua.bind_session(session);
        ASSERT_TRUE(lua.run_file(std::string(PROWSETK_SOURCE_DIR) +
            "/examples/booking-dotcom-admin-scrape/scrape-booking-dotcom-admin.lua").ok);
        const auto prepared = lua.call_function("prepare_session", {
            {"url", "url", "https://admin.booking.com/start"}, {"dotenv", "path", dotenv.string()},
            {"success_beacon", "string", "xpath=//h1[contains(normalize-space(.), 'Joe Litty Rooms')]"},
            {"success_beacon_type", "string", "xpath"}});
        EXPECT_EQ(prepared.ok, valid);
        if (valid) {
            EXPECT_EQ(session->current_url(), "https://admin.booking.com/dashboard");
            prowsetk::MemoryNetworkClient agent;
            agent.set_handler([](const auto& request) {
                return response(request.url.ends_with("/session") ? R"({"id":"agent"})" : answer("stop"));
            });
            b::OpenCodeClient client(agent, b::BridgeConfig{});
            auto result = m::run(*session, client, m::parse_decisions(R"({"version":1,"goal":"inspect","actions":[]})"));
            EXPECT_NE(result.extraction.openapi_yaml.find("integer"), std::string::npos);
            EXPECT_FALSE(transport->requests().empty());
            EXPECT_EQ(result.extraction.openapi_yaml.find("booking-private"), std::string::npos);
        }
    }
    std::filesystem::remove(dotenv);
}
