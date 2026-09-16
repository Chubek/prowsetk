#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "prowsetk/network_client.hpp"
#include "prowsetk/web_interface.hpp"

using prowsetk::HttpResponse;
using prowsetk::MemoryNetworkClient;
using prowsetk::WebInterface;
using prowsetk::WebInterfaceConfig;
using prowsetk::WebRequest;
using prowsetk::WebResponse;

namespace {

const char* kHtml = R"HTML(<html><head><title>Example</title></head><body>
  <h1>Hello</h1>
  <a href="/about">About</a>
  <form action="/api/login" method="POST"><input name="user"></form>
  <script>fetch('/api/items?page=1');</script>
</body></html>)HTML";

WebRequest request(std::string method, std::string path,
                   std::string body = {}) {
    WebRequest request;
    request.method = std::move(method);
    request.path = std::move(path);
    request.body = std::move(body);
    return request;
}

std::string body(const WebResponse& response) {
    return response.body;
}

// A web interface whose browser fetches from an in-memory client, keeping the
// tests hermetic (no sockets, no real network).
std::unique_ptr<WebInterface> make_interface(
    std::string html = std::string(kHtml)) {
    WebInterfaceConfig config;
    config.browser.javascript = false;
    auto interface = std::make_unique<WebInterface>(std::move(config));

    auto network = std::make_unique<MemoryNetworkClient>();
    HttpResponse response;
    response.status = 200;
    response.body = std::move(html);
    response.final_url = "https://example.com/";
    response.headers.emplace_back("Content-Type", "text/html");
    network->set_response("https://example.com/", std::move(response));

    interface->browser().set_network_client(std::move(network));
    return interface;
}

}  // namespace

TEST(WebInterface, HealthReportsVersionAndEngine) {
    auto interface = make_interface();
    const WebResponse response =
        interface->handle(request("GET", "/api/health"));
    EXPECT_EQ(response.status, 200);
    EXPECT_NE(body(response).find("\"engine\":\"Flatworm\""), std::string::npos);
    EXPECT_NE(body(response).find("\"version\":\""), std::string::npos);
}

TEST(WebInterface, CapabilitiesAreReported) {
    auto interface = make_interface();
    const WebResponse response =
        interface->handle(request("GET", "/api/capabilities"));
    EXPECT_EQ(response.status, 200);
    EXPECT_NE(body(response).find("\"name\":\"engine\""), std::string::npos);
    EXPECT_NE(body(response).find("\"name\":\"endpoint-extraction\""),
              std::string::npos);
}

TEST(WebInterface, SessionLifecycle) {
    auto interface = make_interface();

    const WebResponse created =
        interface->handle(request("POST", "/api/sessions", "{}"));
    EXPECT_EQ(created.status, 201);
    EXPECT_NE(body(created).find("\"id\":\"session-1\""), std::string::npos);

    const WebResponse listed =
        interface->handle(request("GET", "/api/sessions"));
    EXPECT_EQ(listed.status, 200);
    EXPECT_NE(body(listed).find("\"id\":\"session-1\""), std::string::npos);

    const WebResponse removed = interface->handle(
        request("DELETE", "/api/sessions/session-1"));
    EXPECT_EQ(removed.status, 200);
    EXPECT_NE(body(removed).find("\"ok\":true"), std::string::npos);

    const WebResponse missing =
        interface->handle(request("GET", "/api/sessions/session-1"));
    EXPECT_EQ(missing.status, 404);
}

TEST(WebInterface, NavigateLoadsDocument) {
    auto interface = make_interface();
    interface->handle(request("POST", "/api/sessions", "{}"));

    const WebResponse response = interface->handle(
        request("POST", "/api/sessions/session-1/navigate",
                "{\"url\":\"https://example.com/\"}"));
    EXPECT_EQ(response.status, 200);
    EXPECT_NE(body(response).find("\"title\":\"Example\""), std::string::npos);
    EXPECT_NE(body(response).find("\"url\":\"https://example.com/\""),
              std::string::npos);
}

TEST(WebInterface, ScrapeExtractsElementsBySelector) {
    auto interface = make_interface();
    interface->handle(request("POST", "/api/sessions", "{}"));

    const WebResponse response = interface->handle(
        request("POST", "/api/sessions/session-1/scrape",
                "{\"url\":\"https://example.com/\","
                "\"selectors\":[\"h1\",\"a\"]}"));
    EXPECT_EQ(response.status, 200);
    EXPECT_NE(body(response).find("\"selector\":\"h1\""), std::string::npos);
    EXPECT_NE(body(response).find("\"count\":1"), std::string::npos);
    EXPECT_NE(body(response).find("\"text\":\"Hello\""), std::string::npos);
    EXPECT_NE(body(response).find("\"text\":\"About\""), std::string::npos);
}

TEST(WebInterface, LinksAreDiscovered) {
    auto interface = make_interface();
    interface->handle(request("POST", "/api/sessions", "{}"));

    const WebResponse response = interface->handle(
        request("POST", "/api/sessions/session-1/links",
                "{\"url\":\"https://example.com/\"}"));
    EXPECT_EQ(response.status, 200);
    EXPECT_NE(body(response).find("\"href\":\"/about\""), std::string::npos);
}

TEST(WebInterface, EndpointExtractionReturnsOpenApi) {
    auto interface = make_interface();
    interface->handle(request("POST", "/api/sessions", "{}"));

    const WebResponse response = interface->handle(
        request("POST", "/api/sessions/session-1/endpoints",
                "{\"url\":\"https://example.com/\","
                "\"observe_network\":true}"));
    EXPECT_EQ(response.status, 200);
    EXPECT_NE(body(response).find("\"openapi_yaml\""), std::string::npos);
    EXPECT_NE(body(response).find("\"/api/login\""), std::string::npos);
    EXPECT_NE(body(response).find("\"/api/items\""), std::string::npos);
    // The navigated URL is reported as an observed endpoint with provenance.
    EXPECT_NE(body(response).find("\"observed-network\""), std::string::npos);
}

TEST(WebInterface, MalformedJsonReturns400) {
    auto interface = make_interface();
    interface->handle(request("POST", "/api/sessions", "{}"));

    const WebResponse response = interface->handle(
        request("POST", "/api/sessions/session-1/scrape", "{not json"));
    EXPECT_EQ(response.status, 400);
    EXPECT_NE(body(response).find("\"error\""), std::string::npos);
}

TEST(WebInterface, UnknownSessionReturns404) {
    auto interface = make_interface();
    const WebResponse response =
        interface->handle(request("GET", "/api/sessions/does-not-exist"));
    EXPECT_EQ(response.status, 404);
}

TEST(WebInterface, UnsupportedMethodReturns405) {
    auto interface = make_interface();
    interface->handle(request("POST", "/api/sessions", "{}"));

    const WebResponse response = interface->handle(
        request("GET", "/api/sessions/session-1/navigate"));
    EXPECT_EQ(response.status, 405);
}

TEST(WebInterface, NavigationFailureReturns502) {
    auto interface = make_interface();
    interface->handle(request("POST", "/api/sessions", "{}"));

    // No response registered for this URL: the memory client throws NotFound,
    // which surfaces as a >= 400 error from the navigation handler.
    const WebResponse response = interface->handle(
        request("POST", "/api/sessions/session-1/navigate",
                "{\"url\":\"https://unregistered.invalid/\"}"));
    EXPECT_GE(response.status, 400);
    EXPECT_NE(body(response).find("\"error\""), std::string::npos);
}

TEST(WebInterface, EvaluateJavaScriptDisabledReturns501) {
    auto interface = make_interface();
    interface->handle(request("POST", "/api/sessions", "{}"));

    const WebResponse response = interface->handle(
        request("POST", "/api/sessions/session-1/evaluate",
                "{\"script\":\"1 + 1\"}"));
    EXPECT_EQ(response.status, 501);
}

TEST(WebInterface, EmptyWebRootServesJsonOnly) {
    WebInterfaceConfig config;
    config.browser.javascript = false;
    WebInterface interface(std::move(config));

    const WebResponse response = interface.handle(request("GET", "/"));
    EXPECT_EQ(response.status, 404);
}

TEST(WebInterface, StaticFileIsServedFromWebRoot) {
    namespace fs = std::filesystem;
    const fs::path root =
        fs::temp_directory_path() / "prowsetk_web_test_root";
    fs::remove_all(root);
    fs::create_directories(root);
    {
        std::ofstream out(root / "index.html", std::ios::binary);
        out << "<html><body>served</body></html>";
    }

    WebInterfaceConfig config;
    config.browser.javascript = false;
    config.web_root = root;
    WebInterface interface(std::move(config));

    const WebResponse response = interface.handle(request("GET", "/"));
    EXPECT_EQ(response.status, 200);
    EXPECT_NE(body(response).find("served"), std::string::npos);

    // Path traversal is rejected regardless of web root contents.
    const WebResponse traversal =
        interface.handle(request("GET", "/../secret"));
    EXPECT_EQ(traversal.status, 404);

    fs::remove_all(root);
}
