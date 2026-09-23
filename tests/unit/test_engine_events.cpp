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

TEST(EngineEvents, PageScriptPostSurvivesLoadAndIsObservable) {
    BrowserConfig config;
    config.javascript = true;
    Browser browser(config);
    auto client = std::make_unique<MemoryNetworkClient>();
    const char* html =
        "<html><body><div id='app'></div><script>"
        "document.addEventListener('DOMContentLoaded', function () {"
        "  document.getElementById('app').innerHTML = "
        "    \"<form method='post' action='/api/orders'><input name='q' value='1'></form>\";"
        "  fetch('/api/orders', {method: 'POST', body: 'q=1'});"
        "});"
        "</script></body></html>";
    register_page(*client, html);
    browser.set_network_client(std::move(client));

    auto session = browser.create_session();
    session->load_html(html, "https://admin.example/hotel/hoteladmin");

    const auto form = session->document()->query_selector("form");
    ASSERT_NE(form, nullptr);
    EXPECT_EQ(form->attribute("method"), "post");

    bool saw_post = false;
    for (const auto& call : session->page_script_requests()) {
        if (call.method == "POST" &&
            call.url.find("/api/orders") != std::string::npos) {
            saw_post = true;
            EXPECT_EQ(call.status, 200);
        }
    }
    EXPECT_TRUE(saw_post);

    EndpointExtractionOptions options;
    options.observe_network = true;
    options.minimum_confidence = 0.5;
    EndpointExtractor extractor(options);
    for (const auto& call : session->page_script_requests()) {
        extractor.observe(call.method, call.url, call.status, call.content_type);
    }
    const auto result = extractor.extract(*session->document());
    bool post_endpoint = false;
    for (const auto& endpoint : result.endpoints) {
        if (endpoint.method == "post" &&
            endpoint.path.find("orders") != std::string::npos) {
            post_endpoint = true;
        }
    }
    EXPECT_TRUE(post_endpoint);
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

TEST(SessionScripts, ExternalScriptPostIsScannedStatically) {
    BrowserConfig config;
    config.javascript = true;
    Browser browser(config);
    auto client = std::make_unique<MemoryNetworkClient>();
    client->set_handler([](const HttpRequest& request) {
        HttpResponse response;
        response.status = 200;
        if (request.url == "http://example.com/") {
            response.body =
                "<html><head><script src='/bundle.js'></script></head>"
                "<body><a href='/api/list'>list</a></body></html>";
        } else if (request.url == "http://example.com/bundle.js") {
            // Never executed on load (function is defined but not called),
            // so only static scanning can surface the POST.
            response.headers.emplace_back("Content-Type",
                                          "application/javascript");
            response.body =
                "function submitOrder() {"
                "  return fetch('/api/bundle-orders', {method: 'POST',"
                "    body: 'q=1'});"
                "}";
        } else {
            response.status = 404;
        }
        return response;
    });
    browser.set_network_client(std::move(client));

    auto session = browser.create_session();
    session->navigate("http://example.com/");

    ASSERT_FALSE(session->page_script_texts().empty());
    EndpointExtractionOptions options;
    options.observe_network = false;
    options.inspect_scripts = true;
    options.minimum_confidence = 0.5;
    EndpointExtractor extractor(options);
    for (const auto& text : session->page_script_texts()) {
        extractor.observe_script(text.url, text.body);
    }
    const auto result = extractor.extract(*session->document());
    bool saw_post = false;
    for (const auto& endpoint : result.endpoints) {
        if (endpoint.method == "post" &&
            endpoint.path.find("bundle-orders") != std::string::npos) {
            saw_post = true;
            EXPECT_EQ(endpoint.discovery_method, "inline-script");
        }
    }
    EXPECT_TRUE(saw_post);
}

TEST(SessionScripts, ScriptPostSurvivesScriptNavigation) {
    BrowserConfig config;
    config.javascript = true;
    Browser browser(config);
    auto client = std::make_unique<MemoryNetworkClient>();
    client->set_handler([](const HttpRequest& request) {
        HttpResponse response;
        response.status = 200;
        response.headers.emplace_back("Content-Type", "text/html");
        if (request.url == "http://example.com/") {
            response.body =
                "<html><body><script>"
                "fetch('/api/orders', {method: 'POST', body: 'q=1'});"
                "location.assign('/next');"
                "</script></body></html>";
        } else if (request.url == "http://example.com/next") {
            response.body = "<html><body>next</body></html>";
        } else if (request.url.find("/api/orders") != std::string::npos) {
            response.body = "{}";
        } else {
            response.status = 404;
        }
        return response;
    });
    browser.set_network_client(std::move(client));

    auto session = browser.create_session();
    session->navigate("http://example.com/");
    EXPECT_EQ(session->current_url(), "http://example.com/next");
    bool saw_post = false;
    for (const auto& call : session->page_script_requests()) {
        if (call.method == "POST" &&
            call.url.find("/api/orders") != std::string::npos) {
            saw_post = true;
        }
    }
    EXPECT_TRUE(saw_post);
}