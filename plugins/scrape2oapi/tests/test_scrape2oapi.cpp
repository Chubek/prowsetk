#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "prowsetk/browser.hpp"
#include "prowsetk/network_client.hpp"
#include "prowsetk/plugins/scrape2oapi.hpp"

namespace scrape = prowsetk::plugins::scrape2oapi;

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

scrape::ResolvedEndpoint resolved(std::string path = "/api/items",
                                 std::string method = "get") {
    scrape::ResolvedEndpoint value;
    value.endpoint = endpoint(std::move(path), std::move(method));
    value.final_url = value.endpoint.url;
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

scrape::Scrape2OapiResult scrape_html(const std::string& html,
                                      scrape::Scrape2OapiOptions options = {}) {
    Browser browser;
    auto session = browser.create_session();
    options.html = html;
    options.base_url = "https://example.test/app";
    return scrape::scrape(*session, options);
}

}  // namespace

TEST(Scrape2OapiApiPath, RejectsEmptyPath) {
    EXPECT_FALSE(scrape::is_api_path("", {}));
}

TEST(Scrape2OapiApiPath, RecognizesApiPrefix) {
    EXPECT_TRUE(scrape::is_api_path("/api/users", {}));
}

TEST(Scrape2OapiApiPath, RecognizesVersionOne) {
    EXPECT_TRUE(scrape::is_api_path("/v1/users", {}));
}

TEST(Scrape2OapiApiPath, RecognizesVersionTwo) {
    EXPECT_TRUE(scrape::is_api_path("/v2/users", {}));
}

TEST(Scrape2OapiApiPath, RecognizesVersionThree) {
    EXPECT_TRUE(scrape::is_api_path("/v3/users", {}));
}

TEST(Scrape2OapiApiPath, RecognizesGraphql) {
    EXPECT_TRUE(scrape::is_api_path("/graphql", {}));
}

TEST(Scrape2OapiApiPath, RecognizesRest) {
    EXPECT_TRUE(scrape::is_api_path("/rest/search", {}));
}

TEST(Scrape2OapiApiPath, RecognizesRpcFallback) {
    EXPECT_TRUE(scrape::is_api_path("/rpc/execute", {}));
}

TEST(Scrape2OapiApiPath, RecognizesJsonFallback) {
    EXPECT_TRUE(scrape::is_api_path("/metadata.json", {}));
}

TEST(Scrape2OapiApiPath, RecognizesDataFallback) {
    EXPECT_TRUE(scrape::is_api_path("/data/export", {}));
}

TEST(Scrape2OapiApiPath, RecognizesInternalFallback) {
    EXPECT_TRUE(scrape::is_api_path("/internal/health", {}));
}

TEST(Scrape2OapiApiPath, RecognizesPrivateFallback) {
    EXPECT_TRUE(scrape::is_api_path("/private/health", {}));
}

TEST(Scrape2OapiApiPath, RecognizesBookingAdminFallbacks) {
    EXPECT_TRUE(scrape::is_api_path("/hotel/hoteladmin", {}));
    EXPECT_TRUE(scrape::is_api_path("/partner-settings/security", {}));
}

TEST(Scrape2OapiApiPath, IsCaseInsensitive) {
    EXPECT_TRUE(scrape::is_api_path("/API/Users", {}));
}

TEST(Scrape2OapiApiPath, AcceptsCustomPattern) {
    EXPECT_TRUE(scrape::is_api_path("/service/orders", {"/service"}));
}

TEST(Scrape2OapiApiPath, CustomPatternIsCaseInsensitive) {
    EXPECT_TRUE(scrape::is_api_path("/SERVICE/orders", {"/service"}));
}

TEST(Scrape2OapiApiPath, IgnoresEmptyCustomPattern) {
    EXPECT_FALSE(scrape::is_api_path("/products", {""}));
}

TEST(Scrape2OapiApiPath, RejectsOrdinaryPage) {
    EXPECT_FALSE(scrape::is_api_path("/products", {}));
}

TEST(Scrape2OapiFilter, KeepsApiEndpointByDefault) {
    auto values = scrape::filter_to_api({endpoint()}, {});
    ASSERT_EQ(values.size(), 1u);
}

TEST(Scrape2OapiFilter, DropsOrdinaryEndpointByDefault) {
    auto values = scrape::filter_to_api({endpoint("/home")}, {});
    EXPECT_TRUE(values.empty());
}

TEST(Scrape2OapiFilter, PreservesOrder) {
    auto values = scrape::filter_to_api(
        {endpoint("/api/first"), endpoint("/home"), endpoint("/v1/second")}, {});
    ASSERT_EQ(values.size(), 2u);
    EXPECT_EQ(values[0].path, "/api/first");
    EXPECT_EQ(values[1].path, "/v1/second");
}

TEST(Scrape2OapiFilter, CanDisablePatternRequirement) {
    scrape::Scrape2OapiOptions options;
    options.require_api_pattern = false;
    EXPECT_EQ(scrape::filter_to_api({endpoint("/home")}, options).size(), 1u);
}

TEST(Scrape2OapiFilter, HonorsCustomPattern) {
    scrape::Scrape2OapiOptions options;
    options.api_patterns = {"/service"};
    EXPECT_EQ(scrape::filter_to_api({endpoint("/service/users")}, options).size(), 1u);
}

TEST(Scrape2OapiFilter, BuiltinMarkersRemainFallbackWithCustomPatterns) {
    scrape::Scrape2OapiOptions options;
    options.api_patterns = {"/service"};
    EXPECT_EQ(scrape::filter_to_api({endpoint("/graphql")}, options).size(), 1u);
}

TEST(Scrape2OapiFilter, DropsNonmatchingCustomPath) {
    scrape::Scrape2OapiOptions options;
    options.api_patterns = {"/service"};
    EXPECT_TRUE(scrape::filter_to_api({endpoint("/home")}, options).empty());
}

TEST(Scrape2OapiFilter, RetainsEndpointMetadata) {
    auto original = endpoint();
    original.parameters = {"q"};
    auto values = scrape::filter_to_api({original}, {});
    ASSERT_EQ(values.size(), 1u);
    EXPECT_EQ(values[0].parameters, original.parameters);
}

TEST(Scrape2OapiFilter, HandlesEmptyInput) {
    EXPECT_TRUE(scrape::filter_to_api({}, {}).empty());
}

TEST(Scrape2OapiYaml, EmitsOpenApiVersion) {
    scrape::Scrape2OapiOptions options;
    options.openapi_version = "3.0.3";
    EXPECT_NE(scrape::render_scrape_yaml({}, options, {}).find("openapi: 3.0.3"),
              std::string::npos);
}

TEST(Scrape2OapiYaml, EmitsEmptyPaths) {
    EXPECT_NE(scrape::render_scrape_yaml({}, {}, {}).find("paths: {}"),
              std::string::npos);
}

TEST(Scrape2OapiYaml, EmitsPluginMarker) {
    EXPECT_NE(scrape::render_scrape_yaml({}, {}, {}).find("x-prowsetk-plugin: scrape2oapi"),
              std::string::npos);
}

TEST(Scrape2OapiYaml, EmitsPath) {
    EXPECT_NE(scrape::render_scrape_yaml({resolved()}, {}, {}).find("'/api/items':"),
              std::string::npos);
}

TEST(Scrape2OapiYaml, EmitsMethod) {
    EXPECT_NE(scrape::render_scrape_yaml({resolved("/api/items", "post")}, {}, {}).find("    post:"),
              std::string::npos);
}

TEST(Scrape2OapiYaml, EmitsOperationId) {
    EXPECT_NE(scrape::render_scrape_yaml({resolved("/api/items", "post")}, {}, {}).find("operationId: post_api_items"),
              std::string::npos);
}

TEST(Scrape2OapiYaml, QuotesApostrophesInPath) {
    EXPECT_NE(scrape::render_scrape_yaml({resolved("/api/o'reilly")}, {}, {}).find("'/api/o''reilly':"),
              std::string::npos);
}

TEST(Scrape2OapiYaml, EmitsQueryParameter) {
    auto value = resolved();
    value.endpoint.parameters = {"q"};
    EXPECT_NE(scrape::render_scrape_yaml({value}, {}, {}).find("name: 'q'"),
              std::string::npos);
}

TEST(Scrape2OapiYaml, QuotesApostrophesInParameter) {
    auto value = resolved();
    value.endpoint.parameters = {"owner's-id"};
    EXPECT_NE(scrape::render_scrape_yaml({value}, {}, {}).find("name: 'owner''s-id'"),
              std::string::npos);
}

TEST(Scrape2OapiYaml, EmitsRequestBodyContentType) {
    auto value = resolved();
    value.endpoint.request_content_type = "application/json";
    EXPECT_NE(scrape::render_scrape_yaml({value}, {}, {}).find("'application/json':"),
              std::string::npos);
}

TEST(Scrape2OapiYaml, EmitsResponseContentType) {
    auto value = resolved();
    value.content_type = "application/json";
    EXPECT_NE(scrape::render_scrape_yaml({value}, {}, {}).find("'application/json':"),
              std::string::npos);
}

TEST(Scrape2OapiYaml, UsesObservedResponseStatus) {
    auto value = resolved();
    value.status = 201;
    EXPECT_NE(scrape::render_scrape_yaml({value}, {}, {}).find("'201':"),
              std::string::npos);
}

TEST(Scrape2OapiYaml, EmitsInferredMarkerWhenEnabled) {
    EXPECT_NE(scrape::render_scrape_yaml({resolved()}, {}, {}).find("x-inferred: true"),
              std::string::npos);
}

TEST(Scrape2OapiYaml, OmitsInferredMarkerWhenDisabled) {
    scrape::Scrape2OapiOptions options;
    options.infer_schemas = false;
    EXPECT_EQ(scrape::render_scrape_yaml({resolved()}, options, {}).find("x-inferred: true"),
              std::string::npos);
}

TEST(Scrape2OapiYaml, EmitsProvenanceWhenEnabled) {
    EXPECT_NE(scrape::render_scrape_yaml({resolved()}, {}, {}).find("x-prowsetk-provenance:"),
              std::string::npos);
}

TEST(Scrape2OapiYaml, OmitsProvenanceWhenDisabled) {
    scrape::Scrape2OapiOptions options;
    options.include_provenance = false;
    EXPECT_EQ(scrape::render_scrape_yaml({resolved()}, options, {}).find("x-prowsetk-provenance:"),
              std::string::npos);
}

TEST(Scrape2OapiYaml, RedactsProvenanceSecretsByDefault) {
    auto value = resolved();
    value.endpoint.url += "?api_key=supersecret";
    const auto yaml = scrape::render_scrape_yaml({value}, {}, {});
    EXPECT_EQ(yaml.find("supersecret"), std::string::npos);
}

TEST(Scrape2OapiYaml, CanExposeProvenanceSecretsOnRequest) {
    auto value = resolved();
    value.endpoint.url += "?api_key=visible";
    scrape::Scrape2OapiOptions options;
    options.redact_secrets = false;
    EXPECT_NE(scrape::render_scrape_yaml({value}, options, {}).find("visible"),
              std::string::npos);
}

TEST(Scrape2OapiYaml, EmitsRedirectChain) {
    auto value = resolved();
    value.redirect_chain = {"https://example.test/start", "https://example.test/api/items"};
    EXPECT_NE(scrape::render_scrape_yaml({value}, {}, {}).find("redirect-chain:"),
              std::string::npos);
}

TEST(Scrape2OapiYaml, EmitsChainWithoutProvenance) {
    auto value = resolved();
    value.redirect_chain = {"https://example.test/api/items"};
    scrape::Scrape2OapiOptions options;
    options.resolve_chain = true;
    options.include_provenance = false;
    EXPECT_NE(scrape::render_scrape_yaml({value}, options, {}).find("x-prowsetk-chain:"),
              std::string::npos);
}

TEST(Scrape2OapiYaml, SortsPathsDeterministically) {
    const auto yaml = scrape::render_scrape_yaml(
        {resolved("/v1/z"), resolved("/api/a")}, {}, {});
    EXPECT_LT(yaml.find("'/api/a':"), yaml.find("'/v1/z':"));
}

TEST(Scrape2OapiYaml, EmitsResolutionError) {
    auto value = resolved();
    value.error = "denied";
    EXPECT_NE(scrape::render_scrape_yaml({value}, {}, {}).find("resolve-error: 'denied'"),
              std::string::npos);
}

TEST(Scrape2OapiDocument, FindsScriptFetch) {
    const auto result = scrape_html("<script>fetch('/api/users')</script>");
    ASSERT_EQ(result.endpoints.size(), 1u);
    EXPECT_EQ(result.endpoints[0].path, "/api/users");
}

TEST(Scrape2OapiDocument, FiltersOrdinaryLinks) {
    const auto result = scrape_html("<a href='/home'>Home</a>");
    EXPECT_TRUE(result.endpoints.empty());
}

TEST(Scrape2OapiDocument, FindsApiLinks) {
    const auto result = scrape_html(
        "<form action='/api/users' method='GET'><input name='q'></form>");
    ASSERT_EQ(result.endpoints.size(), 1u);
    EXPECT_EQ(result.endpoints[0].path, "/api/users");
}

TEST(Scrape2OapiDocument, SupportsCustomPattern) {
    scrape::Scrape2OapiOptions options;
    options.api_patterns = {"/service"};
    const auto result = scrape_html(
        "<form action='/api/users' method='GET'><input name='q'></form>", options);
    ASSERT_EQ(result.endpoints.size(), 1u);
}

TEST(Scrape2OapiDocument, RespectsDisabledScriptInspection) {
    scrape::Scrape2OapiOptions options;
    options.inspect_scripts = false;
    const auto result = scrape_html("<script>fetch('/api/users')</script>", options);
    EXPECT_TRUE(result.endpoints.empty());
}

TEST(Scrape2OapiDocument, KeepsProvenanceInYaml) {
    const auto result = scrape_html("<script>fetch('/api/users')</script>");
    EXPECT_NE(result.openapi_yaml.find("x-prowsetk-provenance:"), std::string::npos);
}

TEST(Scrape2OapiDocument, ProducesDeterministicYaml) {
    const auto first = scrape_html("<script>fetch('/api/users')</script>");
    const auto second = scrape_html("<script>fetch('/api/users')</script>");
    EXPECT_EQ(first.openapi_yaml, second.openapi_yaml);
}

TEST(Scrape2OapiDocument, HonorsConfidenceThreshold) {
    scrape::Scrape2OapiOptions options;
    options.minimum_confidence = 1.0;
    const auto result = scrape_html("<script>fetch('/api/users')</script>", options);
    EXPECT_TRUE(result.endpoints.empty());
}

TEST(Scrape2OapiSession, ReportsMissingDocument) {
    Browser browser;
    auto session = browser.create_session();
    const auto result = scrape::scrape_from_session(*session, {});
    ASSERT_EQ(result.warnings.size(), 1u);
    EXPECT_EQ(result.warnings[0], "session has no document");
}

TEST(Scrape2OapiSession, RecommendsAssistantBrowserWhenEnabledAndNoEndpoints) {
    Browser browser;
    auto session = browser.create_session();
    session->load_html("<html><title>Verification required</title><p>captcha</p></html>",
                       "https://example.test/login");
    scrape::Scrape2OapiOptions options;
    options.assistant_browser_enabled = true;
    options.assistant_browser = "assistant-browser";
    options.assistant_browser_method = "cdp";
    options.assistant_browser_endpoint = "http://127.0.0.1:9222";
    options.assistant_browser_debug_port = 9222;
    const auto result = scrape::scrape_from_session(*session, options);
    ASSERT_TRUE(result.assistant_browser.has_value());
    EXPECT_TRUE(result.assistant_browser->needed);
    EXPECT_EQ(result.assistant_browser->command, "assistant-browser");
    EXPECT_EQ(result.assistant_browser->method, "cdp");
    EXPECT_EQ(result.assistant_browser->endpoint, "http://127.0.0.1:9222");
    EXPECT_EQ(result.assistant_browser->debug_port, 9222u);
    ASSERT_FALSE(result.warnings.empty());
    EXPECT_NE(result.warnings.back().find("assistant browser handoff recommended"),
              std::string::npos);
    EXPECT_NE(result.openapi_yaml.find("x-prowsetk-assistant-browser:"),
              std::string::npos);
}

TEST(Scrape2OapiSession, DoesNotRecommendAssistantBrowserWhenDisabled) {
    Browser browser;
    auto session = browser.create_session();
    session->load_html("<p>captcha</p>", "https://example.test/login");
    const auto result = scrape::scrape_from_session(*session, {});
    EXPECT_FALSE(result.assistant_browser.has_value());
}

TEST(Scrape2OapiSession, ResolvesEndpointThroughNetwork) {
    Browser browser;
    auto network = std::make_unique<MemoryNetworkClient>();
    network->set_response("https://example.test/api/users", response(200, "{}"));
    browser.set_network_client(std::move(network));
    auto session = browser.create_session();
    session->load_html("<script>fetch('/api/users')</script>", "https://example.test/app");
    scrape::Scrape2OapiOptions options;
    options.resolve_chain = true;
    const auto result = scrape::scrape_from_session(*session, options);
    ASSERT_EQ(result.resolved.size(), 1u);
    EXPECT_EQ(result.resolved[0].status, 200);
}

TEST(Scrape2OapiSession, FollowsJsonApiLinks) {
    Browser browser;
    auto network = std::make_unique<MemoryNetworkClient>();
    network->set_response("https://example.test/api/first", response(200, "{\"next\":\"/api/second\"}"));
    network->set_response("https://example.test/api/second", response(200, "{}"));
    browser.set_network_client(std::move(network));
    auto session = browser.create_session();
    session->load_html("<script>fetch('/api/first')</script>", "https://example.test/app");
    scrape::Scrape2OapiOptions options;
    options.resolve_chain = true;
    const auto result = scrape::scrape_from_session(*session, options);
    EXPECT_EQ(result.resolved.size(), 2u);
}

TEST(Scrape2OapiSession, HonorsResolutionRequestLimit) {
    Browser browser;
    auto network = std::make_unique<MemoryNetworkClient>();
    network->set_response("https://example.test/api/one", response(200, "{}"));
    network->set_response("https://example.test/api/two", response(200, "{}"));
    auto* observed_network = network.get();
    browser.set_network_client(std::move(network));
    auto session = browser.create_session();
    session->load_html("<script>fetch('/api/one'); fetch('/api/two')</script>", "https://example.test/app");
    // The engine executed both page-script fetches at load; the budget applies
    // to the plugin's own JSON-chain resolution requests.
    const auto page_requests = observed_network->requests().size();
    scrape::Scrape2OapiOptions options;
    options.resolve_chain = true;
    options.max_resolve_requests = 1;
    const auto result = scrape::scrape_from_session(*session, options);
    EXPECT_EQ(observed_network->requests().size() - page_requests, 1u);
    EXPECT_EQ(result.resolved.size(), 2u);
}
