#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "prowsetk/browser.hpp"
#include "prowsetk/network_client.hpp"
#include "prowsetk/plugins/scrape_endpoints.hpp"

namespace scrape = prowsetk::plugins::scrape_endpoints;

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

TEST(Scrape2OapiApiPath, RecognizesTelemetryAndChallengeFallbacks) {
    EXPECT_TRUE(scrape::is_api_path(
        "/__challenge_h78IRKX3kpQxScCExxShBNwRUlb/d8c14d4960ca/"
        "3e0e3952d8f6/telemetry",
        {}));
    EXPECT_TRUE(scrape::is_api_path("/challenge/telemetry", {}));
    EXPECT_TRUE(scrape::is_api_path("/beacon/collect", {}));
    EXPECT_TRUE(scrape::is_api_path("/collect/events", {}));
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

TEST(Scrape2OapiGarbage, RejectsEmptyPath) {
    EXPECT_FALSE(scrape::is_garbage_path("", {}));
}

TEST(Scrape2OapiGarbage, RejectsScriptBundle) {
    EXPECT_TRUE(scrape::is_garbage_path("/static/app.js", {}));
    EXPECT_TRUE(scrape::is_garbage_path("/assets/bundle.mjs", {}));
}

TEST(Scrape2OapiGarbage, RejectsStylesheet) {
    EXPECT_TRUE(scrape::is_garbage_path("/css/site.css", {}));
}

TEST(Scrape2OapiGarbage, RejectsImage) {
    EXPECT_TRUE(scrape::is_garbage_path("/img/logo.png", {}));
    EXPECT_TRUE(scrape::is_garbage_path("/images/photo.jpg", {}));
}

TEST(Scrape2OapiGarbage, RejectsFontAndSourceMap) {
    EXPECT_TRUE(scrape::is_garbage_path("/fonts/icons.woff2", {}));
    EXPECT_TRUE(scrape::is_garbage_path("/static/app.js.map", {}));
}

TEST(Scrape2OapiGarbage, RejectsAssetDirectory) {
    EXPECT_TRUE(scrape::is_garbage_path("/static/chunk.js", {}));
    EXPECT_TRUE(scrape::is_garbage_path("/node_modules/lib/index.js", {}));
}

TEST(Scrape2OapiGarbage, IsCaseInsensitive) {
    EXPECT_TRUE(scrape::is_garbage_path("/STATIC/APP.JS", {}));
}

TEST(Scrape2OapiGarbage, IgnoresQueryStringAndFragment) {
    EXPECT_TRUE(scrape::is_garbage_path("/app.js?v=2", {}));
    EXPECT_FALSE(scrape::is_garbage_path("/page?x=.js", {}));
}

TEST(Scrape2OapiGarbage, RejectsBundleUnderApiPath) {
    EXPECT_TRUE(scrape::is_garbage_path("/api/bundle.js", {}));
}

TEST(Scrape2OapiGarbage, KeepsApiEndpoint) {
    EXPECT_FALSE(scrape::is_garbage_path("/api/users", {}));
}

TEST(Scrape2OapiGarbage, KeepsOrdinaryPage) {
    // Garbage matching is asset-only; plain pages are the allow-rule's job.
    EXPECT_FALSE(scrape::is_garbage_path("/home", {}));
    EXPECT_FALSE(scrape::is_garbage_path("/reservations", {}));
}

TEST(Scrape2OapiFilter, KeepsApiEndpointByDefault) {
    auto values = scrape::filter_to_api({endpoint()}, {});
    ASSERT_EQ(values.size(), 1u);
}

TEST(Scrape2OapiFilter, KeepsPostWithoutApiPatternByDefault) {
    auto values = scrape::filter_to_api(
        {endpoint("/__challenge_abc/telemetry", "post")}, {});
    ASSERT_EQ(values.size(), 1u);
    EXPECT_EQ(values[0].method, "post");
}

TEST(Scrape2OapiFilter, KeepsPutWithoutApiPatternByDefault) {
    auto values = scrape::filter_to_api({endpoint("/settings", "put")}, {});
    ASSERT_EQ(values.size(), 1u);
}

TEST(Scrape2OapiFilter, DropsOrdinaryEndpointByDefault) {
    auto values = scrape::filter_to_api({endpoint("/home")}, {});
    EXPECT_TRUE(values.empty());
}

TEST(Scrape2OapiFilter, DropsGarbageAssetByDefault) {
    EXPECT_TRUE(scrape::filter_to_api({endpoint("/static/app.js")}, {}).empty());
    EXPECT_TRUE(scrape::filter_to_api({endpoint("/img/logo.png")}, {}).empty());
}

TEST(Scrape2OapiFilter, DropsGarbagePostDespiteMethodRule) {
    // A POST to a static asset is gunk, not backend evidence.
    EXPECT_TRUE(
        scrape::filter_to_api({endpoint("/api/pixel.gif", "post")}, {}).empty());
}

TEST(Scrape2OapiFilter, DropsGarbageWithoutPatternRequirement) {
    scrape::Scrape2OapiOptions options;
    options.require_api_pattern = false;
    EXPECT_TRUE(
        scrape::filter_to_api({endpoint("/static/app.js")}, options).empty());
    EXPECT_EQ(scrape::filter_to_api({endpoint("/home")}, options).size(), 1u);
}

TEST(Scrape2OapiFilter, ApiOnlyFalseKeepsEverything) {
    scrape::Scrape2OapiOptions options;
    options.api_only = false;
    EXPECT_EQ(
        scrape::filter_to_api({endpoint("/home"), endpoint("/static/app.js")},
                              options)
            .size(),
        2u);
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
    EXPECT_NE(scrape::render_scrape_yaml({}, {}, {}).find("x-prowsetk-plugin: scrape-endpoints"),
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

TEST(Scrape2OapiDocument, FindsBeaconPostWithoutApiPattern) {
    const auto result = scrape_html(
        "<script>navigator.sendBeacon('/__challenge_abc/telemetry', "
        "JSON.stringify({t: 1}))</script>");
    ASSERT_EQ(result.endpoints.size(), 1u);
    EXPECT_EQ(result.endpoints[0].path, "/__challenge_abc/telemetry");
    EXPECT_EQ(result.endpoints[0].method, "post");
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

TEST(Scrape2OapiSession, ObservesRuntimePostIssuedByPageScript) {
    prowsetk::BrowserConfig config;
    config.javascript = true;
    Browser browser(config);
    auto network = std::make_unique<MemoryNetworkClient>();
    network->set_response("https://example.test/api/orders", response(201, "{}"));
    browser.set_network_client(std::move(network));
    auto session = browser.create_session();
    // Method and path are assembled at runtime inside a lifecycle handler, so
    // static script inspection cannot see them; only execution can.
    session->load_html(
        "<script>document.addEventListener('DOMContentLoaded', function () {"
        "  var m = 'PO' + 'ST';"
        "  fetch('/api/' + 'orders', {method: m, body: 'q=1'});"
        "});</script>",
        "https://example.test/app");

    scrape::Scrape2OapiOptions options;
    options.inspect_scripts = false;
    options.observe_network = true;
    options.minimum_confidence = 0.5;
    const auto observed = scrape::scrape_from_session(*session, options);
    bool saw_post = false;
    for (const auto& endpoint : observed.endpoints) {
        if (endpoint.method == "post" && endpoint.path == "/api/orders") {
            saw_post = true;
        }
    }
    EXPECT_TRUE(saw_post);
    EXPECT_NE(observed.openapi_yaml.find("post:"), std::string::npos);

    options.observe_network = false;
    const auto unobserved = scrape::scrape_from_session(*session, options);
    for (const auto& endpoint : unobserved.endpoints) {
        EXPECT_FALSE(endpoint.method == "post" && endpoint.path == "/api/orders");
    }
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
