#include <gtest/gtest.h>

#include "prowsetk/browser.hpp"
#include "prowsetk/error.hpp"
#include "prowsetk/event.hpp"
#include "prowsetk/network_client.hpp"
#include "prowsetk/url.hpp"

using prowsetk::Browser;
using prowsetk::BrowserConfig;
using prowsetk::Error;
using prowsetk::EventType;
using prowsetk::HttpResponse;
using prowsetk::MemoryNetworkClient;

namespace {

HttpResponse html_response(int status, std::string body,
                           std::string url = {}) {
    HttpResponse response;
    response.status = status;
    response.body = std::move(body);
    response.final_url = std::move(url);
    response.headers.emplace_back("Content-Type", "text/html");
    return response;
}

}  // namespace

TEST(Navigation, LoadsDocumentFromNetwork) {
    Browser browser;
    auto network = std::make_unique<MemoryNetworkClient>();
    network->set_response(
        "https://example.com/",
        html_response(200,
                      "<html><head><title>Networked</title></head>"
                      "<body><h1>Hi</h1></body></html>"));
    browser.set_network_client(std::move(network));

    auto session = browser.create_session();
    session->navigate("https://example.com/");

    ASSERT_NE(session->document(), nullptr);
    EXPECT_EQ(session->document()->title(), "Networked");
    EXPECT_EQ(session->current_url(), "https://example.com/");
    EXPECT_EQ(session->document()->query_selector("h1")->text(), "Hi");
}

TEST(Navigation, FollowsRedirectsAndRecordsChain) {
    Browser browser;
    auto network = std::make_unique<MemoryNetworkClient>();
    HttpResponse redirect;
    redirect.status = 302;
    redirect.headers.emplace_back("Location", "/final");
    network->set_response("https://example.com/start", redirect);
    network->set_response("https://example.com/final",
                          html_response(200, "<title>Arrived</title>"));
    browser.set_network_client(std::move(network));

    auto session = browser.create_session();
    session->navigate("https://example.com/start");

    EXPECT_EQ(session->current_url(), "https://example.com/final");
    EXPECT_EQ(session->document()->title(), "Arrived");
}

TEST(Navigation, HttpErrorsThrow) {
    Browser browser;
    auto network = std::make_unique<MemoryNetworkClient>();
    network->set_response("https://example.com/missing",
                          html_response(404, "not found"));
    browser.set_network_client(std::move(network));

    auto session = browser.create_session();
    EXPECT_THROW(session->navigate("https://example.com/missing"), Error);
}

TEST(Navigation, EmitsLifecycleEvents) {
    Browser browser;
    auto network = std::make_unique<MemoryNetworkClient>();
    network->set_response("https://example.com/",
                          html_response(200, "<title>Events</title>"));
    browser.set_network_client(std::move(network));

    int before_navigation = 0;
    int before_request = 0;
    int after_response = 0;
    int document_created = 0;
    browser.events().subscribe(EventType::BeforeNavigation,
                               [&](prowsetk::Event&) { ++before_navigation; });
    browser.events().subscribe(EventType::BeforeRequest,
                               [&](prowsetk::Event&) { ++before_request; });
    browser.events().subscribe(EventType::AfterResponse,
                               [&](prowsetk::Event&) { ++after_response; });
    browser.events().subscribe(EventType::DocumentCreated,
                               [&](prowsetk::Event&) { ++document_created; });

    auto session = browser.create_session();
    session->navigate("https://example.com/");

    EXPECT_EQ(before_navigation, 1);
    EXPECT_EQ(before_request, 1);
    EXPECT_EQ(after_response, 1);
    EXPECT_EQ(document_created, 1);
}

TEST(Navigation, SendsSessionHeadersAndCookies) {
    Browser browser;
    auto network = std::make_unique<MemoryNetworkClient>();
    network->set_response("https://example.com/",
                          html_response(200, "<title>Headers</title>"));
    auto* network_ptr = network.get();
    browser.set_network_client(std::move(network));

    auto session = browser.create_session();
    session->set_header("X-Trace", "abc");
    prowsetk::Cookie cookie;
    cookie.name = "sid";
    cookie.value = "123";
    session->cookies().set(prowsetk::parse_url("https://example.com/"), cookie);

    session->navigate("https://example.com/");

    ASSERT_EQ(network_ptr->requests().size(), 1u);
    const auto& request = network_ptr->requests()[0];
    bool has_trace = false;
    bool has_cookie = false;
    for (const auto& [name, value] : request.headers) {
        if (name == "X-Trace" && value == "abc") {
            has_trace = true;
        }
        if (name == "Cookie" && value.find("sid=123") != std::string::npos) {
            has_cookie = true;
        }
    }
    EXPECT_TRUE(has_trace);
    EXPECT_TRUE(has_cookie);
}

TEST(Navigation, SessionsHaveIsolatedStorage) {
    Browser browser;
    auto first = browser.create_session();
    auto second = browser.create_session();

    first->local_storage().set("k", "one");
    second->local_storage().set("k", "two");

    ASSERT_TRUE(first->local_storage().get("k").has_value());
    EXPECT_EQ(first->local_storage().get("k").value(), "one");
    EXPECT_EQ(second->local_storage().get("k").value(), "two");
    EXPECT_NE(first->id(), second->id());
}

TEST(Navigation, ReportsUnsupportedJavaScriptWhenEngineAbsent) {
    BrowserConfig config;
    config.javascript = true;
    Browser browser(config);
    auto network = std::make_unique<MemoryNetworkClient>();
    network->set_response(
        "https://example.com/",
        html_response(200, "<title>JS</title><script>var a = 1;</script>"));
    browser.set_network_client(std::move(network));

    int unsupported = 0;
    browser.events().subscribe(EventType::UnsupportedApi,
                               [&](prowsetk::Event&) { ++unsupported; });

    auto session = browser.create_session();
    session->navigate("https://example.com/");

    EXPECT_EQ(unsupported, 1);
    EXPECT_FALSE(browser.capabilities().has("javascript"));
}

TEST(Navigation, CapabilitiesReflectUnavailableWasm) {
    Browser browser;
    const auto capabilities = browser.capabilities();
    EXPECT_TRUE(capabilities.has("html-parser"));
    EXPECT_TRUE(capabilities.has("css-selectors"));
    EXPECT_FALSE(capabilities.has("wasm"));
    EXPECT_FALSE(capabilities.has("wasi"));
}
