#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "prowsetk/browser.hpp"
#include "prowsetk/endpoint_extraction.hpp"
#include "prowsetk/error.hpp"
#include "prowsetk/network_client.hpp"

using prowsetk::Browser;
using prowsetk::BrowserConfig;
using prowsetk::EndpointExtractor;
using prowsetk::EndpointExtractionOptions;
using prowsetk::Error;
using prowsetk::ErrorCode;
using prowsetk::Event;
using prowsetk::EventType;
using prowsetk::HttpRequest;
using prowsetk::HttpResponse;
using prowsetk::MemoryNetworkClient;

namespace {

void register_page(MemoryNetworkClient& client, std::string html) {
    client.set_handler([html = std::move(html)](const HttpRequest& request) {
        HttpResponse response;
        response.status = 200;
        response.headers.emplace_back("Content-Type", "text/html");
        response.body = html;
        (void)request;
        return response;
    });
}

}  // namespace

TEST(EngineEvents, ConsoleEventsFromPageScripts) {
    BrowserConfig config;
    config.javascript = true;
    Browser browser(config);
    auto client = std::make_unique<MemoryNetworkClient>();
    register_page(*client,
                  "<html><body><script>console.log('page-ready', 7);"
                  "</script></body></html>");
    browser.set_network_client(std::move(client));

    auto session = browser.create_session();
    std::vector<Event> console_events;
    session->events().subscribe(EventType::Console,
                                [&](Event& event) { console_events.push_back(event); });
    session->navigate("http://example.com/");

    ASSERT_EQ(console_events.size(), 1u);
    EXPECT_EQ(console_events[0].message, "page-ready 7");
    EXPECT_EQ(console_events[0].name, "log");
    EXPECT_EQ(console_events[0].session_id, session->id());
}

TEST(EngineEvents, StorageAccessEventsAreScopedAndValueFree) {
    Browser browser;
    auto session = browser.create_session();
    std::vector<Event> events;
    session->events().subscribe(EventType::StorageAccess,
                                [&](Event& event) { events.push_back(event); });

    session->local_storage().set("key", "sensitive-value");
    session->local_storage().get("key");
    session->local_storage().remove("key");

    ASSERT_EQ(events.size(), 3u);
    EXPECT_EQ(events[0].name, "set");
    EXPECT_EQ(events[0].attributes["store"], "local");
    EXPECT_EQ(events[0].attributes["key"], "key");
    EXPECT_EQ(events[1].name, "get");
    EXPECT_EQ(events[2].name, "remove");
}

TEST(EngineEvents, EndpointDiscoveredEventsCarryProvenance) {
    Browser browser;
    auto session = browser.create_session();
    session->load_html("<html><a href='/api/users'>users</a></html>",
                       "http://example.com/");

    EndpointExtractionOptions options;
    options.observe_network = false;
    options.minimum_confidence = 0.0;
    EndpointExtractor extractor(options);
    extractor.set_event_dispatcher(&session->events());

    std::vector<Event> events;
    session->events().subscribe(EventType::EndpointDiscovered,
                                [&](Event& event) { events.push_back(event); });

    const auto result = extractor.extract(*session->document());
    ASSERT_EQ(result.endpoints.size(), 1u);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].name, "get /api/users");
    EXPECT_EQ(events[0].attributes["discovery-method"], "html-link");
}

TEST(EngineEvents, UnsupportedApiBehaviorWarnsByDefault) {
    Browser browser;
    std::vector<Event> events;
    browser.events().subscribe(EventType::UnsupportedApi,
                               [&](Event& event) { events.push_back(event); });
    browser.handle_unsupported_api("canvas", "not available");
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].name, "canvas");
}

TEST(EngineEvents, UnsupportedApiBehaviorCanThrow) {
    BrowserConfig config;
    config.unsupported_api_behavior = "exception";
    Browser browser(config);
    EXPECT_THROW(browser.handle_unsupported_api("canvas", "no"), Error);
}

TEST(EngineEvents, UnsupportedApiBehaviorCanBeSilent) {
    BrowserConfig config;
    config.unsupported_api_behavior = "dummy";
    Browser browser(config);
    std::vector<Event> events;
    browser.events().subscribe(EventType::UnsupportedApi,
                               [&](Event&) { events.push_back(Event{}); });
    browser.handle_unsupported_api("canvas", "no");
    EXPECT_TRUE(events.empty());
}

TEST(SessionRequest, IssuesArbitraryMethodsWithBody) {
    Browser browser;
    auto client = std::make_unique<MemoryNetworkClient>();
    client->set_handler([](const HttpRequest& request) {
        HttpResponse response;
        response.status = 201;
        response.body = request.method + ":" + request.body;
        return response;
    });
    browser.set_network_client(std::move(client));

    auto session = browser.create_session();
    HttpRequest request;
    request.method = "POST";
    request.url = "http://example.com/items";
    request.body = "a=1&b=2";
    const HttpResponse response = session->request(std::move(request));
    EXPECT_EQ(response.status, 201);
    EXPECT_EQ(response.body, "POST:a=1&b=2");
}

TEST(SessionRequest, ConvertsPostToGetOn303) {
    Browser browser;
    auto client = std::make_unique<MemoryNetworkClient>();
    client->set_handler([](const HttpRequest& request) {
        HttpResponse response;
        if (request.url.find("/submit") != std::string::npos) {
            response.status = 303;
            response.headers.emplace_back("Location",
                                          "http://example.com/done");
        } else {
            response.status = 200;
            response.body = request.method + ":" + request.body;
        }
        return response;
    });
    browser.set_network_client(std::move(client));

    auto session = browser.create_session();
    HttpRequest request;
    request.method = "POST";
    request.url = "http://example.com/submit";
    request.body = "secret";
    const HttpResponse response = session->request(std::move(request));
    EXPECT_EQ(response.final_url, "http://example.com/done");
    EXPECT_EQ(response.body, "GET:");
}

TEST(SessionScripts, ExecutesExternalScripts) {
    BrowserConfig config;
    config.javascript = true;
    Browser browser(config);
    auto client = std::make_unique<MemoryNetworkClient>();
    client->set_handler([](const HttpRequest& request) {
        HttpResponse response;
        response.status = 200;
        if (request.url == "http://example.com/") {
            response.body =
                "<html><script src='/app.js'></script></html>";
        } else if (request.url == "http://example.com/app.js") {
            response.body = "globalThis.loaded = 40 + 2;";
        } else {
            response.status = 404;
        }
        return response;
    });
    browser.set_network_client(std::move(client));

    auto session = browser.create_session();
    session->navigate("http://example.com/");
    EXPECT_EQ(session->evaluate_js("loaded"), "42");
}