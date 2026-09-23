#include <dlfcn.h>
#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "prowsetk/browser.hpp"
#include "prowsetk/network_client.hpp"
#include "prowsetk/plugins/restful_resolver.hpp"

namespace restful = prowsetk::plugins::restful_resolver;

namespace {

using prowsetk::Browser;
using prowsetk::DiscoveredEndpoint;
using prowsetk::HttpResponse;
using prowsetk::MemoryNetworkClient;

DiscoveredEndpoint endpoint(std::string path = "/api/items",
                            std::string method = "get") {
    DiscoveredEndpoint value;
    value.path = std::move(path);
    value.method = std::move(method);
    value.url = "https://example.test" + value.path;
    value.source = "https://example.test/app";
    value.discovery_method = "test";
    value.confidence = 0.75;
    return value;
}

HttpResponse response(int status, std::string body = {},
                      std::string content_type = "application/json") {
    HttpResponse value;
    value.status = status;
    value.body = std::move(body);
    if (!content_type.empty()) {
        value.headers.emplace_back("Content-Type", std::move(content_type));
    }
    return value;
}

restful::RestfulResolverResult resolve_doc(
    const std::string& html, restful::RestfulResolverOptions options = {}) {
    auto document =
        prowsetk::parse_html(html, "https://example.test/app",
                             "https://example.test/app");
    return restful::resolve_from_document(*document, options);
}

}  // namespace

TEST(RestfulResolverApiPath, RejectsEmptyPath) {
    EXPECT_FALSE(restful::is_restful_api_path("", {}));
}

TEST(RestfulResolverApiPath, RecognizesApiPrefix) {
    EXPECT_TRUE(restful::is_restful_api_path("/api/users", {}));
}

TEST(RestfulResolverApiPath, RecognizesBookingAdminFallbacks) {
    EXPECT_TRUE(restful::is_restful_api_path("/hotel/hoteladmin", {}));
    EXPECT_TRUE(restful::is_restful_api_path("/partner-settings/security", {}));
}

TEST(RestfulResolverApiPath, RejectsOrdinaryPage) {
    EXPECT_FALSE(restful::is_restful_api_path("/products", {}));
}

TEST(RestfulResolverFilter, KeepsApiEndpointByDefault) {
    EXPECT_EQ(restful::filter_to_api({endpoint()}, {}).size(), 1u);
}

TEST(RestfulResolverFilter, DropsOrdinaryEndpointByDefault) {
    EXPECT_TRUE(restful::filter_to_api({endpoint("/home")}, {}).empty());
}

TEST(RestfulResolverFilter, CanDisablePatternRequirement) {
    restful::RestfulResolverOptions options;
    options.require_api_pattern = false;
    EXPECT_EQ(restful::filter_to_api({endpoint("/home")}, options).size(), 1u);
}

TEST(RestfulResolverCompleteness, DetectsPost) {
    EXPECT_TRUE(restful::has_post_endpoint({endpoint("/api/a", "post")}));
    EXPECT_FALSE(restful::has_post_endpoint({endpoint("/api/a", "get")}));
}

TEST(RestfulResolverCompleteness, DetectsGet) {
    EXPECT_TRUE(restful::has_get_endpoint({endpoint("/api/a", "get")}));
    EXPECT_FALSE(restful::has_get_endpoint({endpoint("/api/a", "post")}));
}

TEST(RestfulResolverDocument, SeedPostFormAloneIsIncompleteWithoutGet) {
    const auto result = resolve_doc(
        "<form action='/api/items' method='POST'><input name='name'></form>");
    EXPECT_TRUE(result.has_post);
    EXPECT_FALSE(result.has_get);
    EXPECT_FALSE(result.is_complete);
    EXPECT_EQ(result.request_count, 0u);
}

TEST(RestfulResolverDocument, SeedGetPlusPostIsComplete) {
    const auto result = resolve_doc(
        "<script>fetch('/api/items')</script>"
        "<form action='/api/items' method='POST'><input name='name'></form>");
    EXPECT_TRUE(result.has_get);
    EXPECT_TRUE(result.has_post);
    EXPECT_TRUE(result.is_complete);
    EXPECT_EQ(result.request_count, 0u);
}

TEST(RestfulResolverDocument, YamlMarksRestfulExtension) {
    const auto result = resolve_doc(
        "<script>fetch('/api/items')</script>"
        "<form action='/api/items' method='POST'><input name='name'></form>");
    EXPECT_NE(result.openapi_yaml.find("x-prowsetk-restful:"), std::string::npos);
    EXPECT_NE(result.openapi_yaml.find("x-prowsetk-plugin: restful-resolver"),
              std::string::npos);
    EXPECT_NE(result.openapi_yaml.find("is-complete: true"), std::string::npos);
}

TEST(RestfulResolverDocument, YamlRedactsSecretsByDefault) {
    Browser browser;
    auto session = browser.create_session();
    session->load_html("<a href='/api/items?api_key=supersecret'>x</a>",
                       "https://example.test/app");
    restful::RestfulResolverOptions options;
    options.minimum_confidence = 0.30;
    options.require_api_pattern = false;
    const auto result = restful::resolve_from_session(*session, options);
    EXPECT_EQ(result.openapi_yaml.find("supersecret"), std::string::npos);
}

TEST(RestfulResolverSession, ReportsMissingDocument) {
    Browser browser;
    auto session = browser.create_session();
    const auto result = restful::resolve_from_session(*session, {});
    ASSERT_EQ(result.warnings.size(), 1u);
    EXPECT_FALSE(result.is_complete);
}

TEST(RestfulResolverSession, ResolvesUntilPostIsDiscovered) {
    Browser browser;
    auto network = std::make_unique<MemoryNetworkClient>();
    network->set_response("https://example.test/api/items",
                          response(200, "{\"next\":\"/api/items/form\"}"));
    network->set_response(
        "https://example.test/api/items/form",
        response(200,
                 "<form action='/api/items' method='POST'>"
                 "<input name='name'></form>",
                 "text/html"));
    browser.set_network_client(std::move(network));
    auto session = browser.create_session();
    session->load_html("<script>fetch('/api/items')</script>",
                       "https://example.test/app");
    restful::RestfulResolverOptions options;
    options.max_rounds = 4;
    options.max_requests = 16;
    const auto result = restful::resolve_from_session(*session, options);
    EXPECT_TRUE(result.has_get);
    EXPECT_TRUE(result.has_post);
    EXPECT_TRUE(result.is_complete);
    EXPECT_GE(result.request_count, 1u);
    EXPECT_NE(result.openapi_yaml.find("post:"), std::string::npos);
}

TEST(RestfulResolverSession, IncompleteWhenBudgetExhausted) {
    Browser browser;
    auto network = std::make_unique<MemoryNetworkClient>();
    network->set_response("https://example.test/api/items",
                          response(200, "{}"));
    browser.set_network_client(std::move(network));
    auto session = browser.create_session();
    session->load_html("<script>fetch('/api/items')</script>",
                       "https://example.test/app");
    restful::RestfulResolverOptions options;
    options.max_rounds = 2;
    options.max_requests = 4;
    const auto result = restful::resolve_from_session(*session, options);
    EXPECT_TRUE(result.has_get);
    EXPECT_FALSE(result.has_post);
    EXPECT_FALSE(result.is_complete);
    EXPECT_FALSE(result.warnings.empty());
    EXPECT_NE(result.openapi_yaml.find("is-complete: false"), std::string::npos);
}

TEST(RestfulResolverSession, HonorsRequestLimit) {
    Browser browser;
    auto network = std::make_unique<MemoryNetworkClient>();
    network->set_response("https://example.test/api/one", response(200, "{}"));
    network->set_response("https://example.test/api/two", response(200, "{}"));
    auto* observed = network.get();
    browser.set_network_client(std::move(network));
    auto session = browser.create_session();
    session->load_html(
        "<script>fetch('/api/one'); fetch('/api/two')</script>",
        "https://example.test/app");
    const auto page_requests = observed->requests().size();
    restful::RestfulResolverOptions options;
    options.max_requests = 1;
    const auto result = restful::resolve_from_session(*session, options);
    EXPECT_EQ(observed->requests().size() - page_requests, 1u);
    EXPECT_FALSE(result.is_complete);
}

TEST(RestfulResolverSession, RejectsCrossOriginByDefault) {
    Browser browser;
    auto network = std::make_unique<MemoryNetworkClient>();
    network->set_response("https://other.test/api/items", response(200, "{}"));
    browser.set_network_client(std::move(network));
    auto session = browser.create_session();
    session->load_html("<a href='https://other.test/api/items'>x</a>",
                       "https://example.test/app");
    restful::RestfulResolverOptions options;
    options.minimum_confidence = 0.30;
    const auto result = restful::resolve_from_session(*session, options);
    // Cross-origin seed is filtered by the resolver allowlist.
    EXPECT_TRUE(result.endpoints.empty());
}

TEST(RestfulResolverPluginAbi, ExposesEntrySymbol) {
    // The shared plugin must export prowsetk_plugin_entry and report ABI 2.
    void* handle = dlopen(RESTFUL_RESOLVER_PLUGIN_PATH, RTLD_NOW);
    ASSERT_NE(handle, nullptr) << dlerror();
    using Entry = const void* (*)();
    auto entry = reinterpret_cast<Entry>(dlsym(handle, "prowsetk_plugin_entry"));
    ASSERT_NE(entry, nullptr);
    EXPECT_NE(entry(), nullptr);
    dlclose(handle);
}
