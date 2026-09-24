#include <dlfcn.h>
#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "prowsetk/browser.hpp"
#include "prowsetk/network_client.hpp"
#include "prowsetk/plugins/schema_grabber.hpp"

namespace grabber = prowsetk::plugins::schema_grabber;

namespace {

using prowsetk::Browser;
using prowsetk::BrowserConfig;
using prowsetk::DiscoveredEndpoint;
using prowsetk::HttpResponse;
using prowsetk::MemoryNetworkClient;
using prowsetk::Redactor;

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

grabber::SchemaGrabberResult grab_html(
    const std::string& html, grabber::SchemaGrabberOptions options = {}) {
    auto document = prowsetk::parse_html(html, "https://example.test/app",
                                         "https://example.test/app");
    return grabber::grab_from_document(*document, options);
}

}  // namespace

TEST(SchemaGrabberApiPath, RejectsEmptyPath) {
    EXPECT_FALSE(grabber::is_schema_api_path("", {}));
}

TEST(SchemaGrabberApiPath, RecognizesApiPrefix) {
    EXPECT_TRUE(grabber::is_schema_api_path("/api/users", {}));
}

TEST(SchemaGrabberApiPath, RecognizesBookingAdminFallbacks) {
    EXPECT_TRUE(grabber::is_schema_api_path("/hotel/hoteladmin", {}));
    EXPECT_TRUE(grabber::is_schema_api_path("/partner-settings/security", {}));
}

TEST(SchemaGrabberApiPath, RecognizesTelemetryFallbacks) {
    EXPECT_TRUE(grabber::is_schema_api_path("/__challenge_abc/telemetry", {}));
    EXPECT_TRUE(grabber::is_schema_api_path("/beacon/collect", {}));
}

TEST(SchemaGrabberApiPath, RejectsOrdinaryPage) {
    EXPECT_FALSE(grabber::is_schema_api_path("/products", {}));
}

TEST(SchemaGrabberFilter, KeepsApiEndpointByDefault) {
    EXPECT_EQ(grabber::filter_to_api({endpoint()}, {}).size(), 1u);
}

TEST(SchemaGrabberFilter, KeepsPostWithoutApiPatternByDefault) {
    const auto values = grabber::filter_to_api(
        {endpoint("/__challenge_abc/telemetry", "post")}, {});
    ASSERT_EQ(values.size(), 1u);
    EXPECT_EQ(values[0].method, "post");
}

TEST(SchemaGrabberFilter, DropsOrdinaryEndpointByDefault) {
    EXPECT_TRUE(grabber::filter_to_api({endpoint("/home")}, {}).empty());
}

TEST(SchemaGrabberFilter, CanDisablePatternRequirement) {
    grabber::SchemaGrabberOptions options;
    options.require_api_pattern = false;
    EXPECT_EQ(grabber::filter_to_api({endpoint("/home")}, options).size(), 1u);
}

TEST(SchemaGrabberTemplatize, LeavesPlainPathAlone) {
    EXPECT_EQ(grabber::templatize_path("/api/users"), "/api/users");
}

TEST(SchemaGrabberTemplatize, TemplatesNumericId) {
    std::vector<grabber::PathParam> params;
    EXPECT_EQ(grabber::templatize_path("/api/users/123", &params),
              "/api/users/{id}");
    ASSERT_EQ(params.size(), 1u);
    EXPECT_EQ(params[0].name, "id");
    EXPECT_EQ(params[0].example, "123");
}

TEST(SchemaGrabberTemplatize, TemplatesUuid) {
    EXPECT_EQ(grabber::templatize_path(
                  "/api/orders/550e8400-e29b-41d4-a716-446655440000"),
              "/api/orders/{id}");
}

TEST(SchemaGrabberTemplatize, KeepsExistingTemplate) {
    EXPECT_EQ(grabber::templatize_path("/api/users/{id}"), "/api/users/{id}");
}

TEST(SchemaGrabberTemplatize, RootStaysRoot) {
    EXPECT_EQ(grabber::templatize_path("/"), "/");
}

TEST(SchemaGrabberScalar, InfersBoolean) {
    EXPECT_EQ(grabber::infer_scalar_type("true"), "boolean");
    EXPECT_EQ(grabber::infer_scalar_type("FALSE"), "boolean");
}

TEST(SchemaGrabberScalar, InfersInteger) {
    EXPECT_EQ(grabber::infer_scalar_type("42"), "integer");
    EXPECT_EQ(grabber::infer_scalar_type("-7"), "integer");
}

TEST(SchemaGrabberScalar, InfersNumber) {
    EXPECT_EQ(grabber::infer_scalar_type("4.5"), "number");
}

TEST(SchemaGrabberScalar, DefaultsToString) {
    EXPECT_EQ(grabber::infer_scalar_type("hello"), "string");
    EXPECT_EQ(grabber::infer_scalar_type(""), "string");
}

TEST(SchemaGrabberQuery, InfersTypesFromExamples) {
    const auto params = grabber::query_params_for(
        "https://example.test/api/users?page=2&limit=4.5&active=true&q=hi",
        Redactor());
    ASSERT_EQ(params.size(), 4u);
    // Sorted by name: active, limit, page, q.
    EXPECT_EQ(params[0].name, "active");
    EXPECT_EQ(params[0].type, "boolean");
    EXPECT_EQ(params[1].name, "limit");
    EXPECT_EQ(params[1].type, "number");
    EXPECT_EQ(params[2].name, "page");
    EXPECT_EQ(params[2].type, "integer");
    EXPECT_EQ(params[2].example, "2");
    EXPECT_EQ(params[3].name, "q");
    EXPECT_EQ(params[3].type, "string");
    for (const auto& p : params) EXPECT_FALSE(p.required);
}

TEST(SchemaGrabberQuery, RedactsSensitiveExamples) {
    const auto params = grabber::query_params_for(
        "https://example.test/api/users?api_key=supersecret&page=1", Redactor());
    ASSERT_EQ(params.size(), 2u);
    EXPECT_EQ(params[0].example, "[REDACTED]");
    EXPECT_TRUE(params[0].sensitive);
    EXPECT_EQ(params[1].example, "1");
}

TEST(SchemaGrabberJson, InfersObjectSchema) {
    grabber::SchemaGrabberOptions options;
    const auto schema = grabber::infer_json_schema(
        R"({"id": 1, "name": "ada", "admin": true, "score": 4.5})", options,
        Redactor());
    ASSERT_TRUE(schema.has_value());
    ASSERT_EQ(schema->properties.size(), 4u);
    // Sorted: admin, id, name, score.
    EXPECT_EQ(schema->properties[0].name, "admin");
    EXPECT_EQ(schema->properties[0].type, "boolean");
    EXPECT_EQ(schema->properties[1].name, "id");
    EXPECT_EQ(schema->properties[1].type, "integer");
    EXPECT_EQ(schema->properties[2].name, "name");
    EXPECT_EQ(schema->properties[2].type, "string");
    EXPECT_EQ(schema->properties[3].type, "number");
}

TEST(SchemaGrabberJson, InfersNestedObjectsAndArrays) {
    grabber::SchemaGrabberOptions options;
    const auto schema = grabber::infer_json_schema(
        R"({"user": {"id": 1}, "tags": ["a", "b"]})", options, Redactor());
    ASSERT_TRUE(schema.has_value());
    ASSERT_EQ(schema->properties.size(), 2u);
    EXPECT_EQ(schema->properties[0].name, "tags");
    EXPECT_EQ(schema->properties[0].type, "array");
    EXPECT_EQ(schema->properties[0].items_type, "string");
    EXPECT_EQ(schema->properties[1].name, "user");
    EXPECT_EQ(schema->properties[1].type, "object");
    ASSERT_EQ(schema->properties[1].properties.size(), 1u);
    EXPECT_EQ(schema->properties[1].properties[0].type, "integer");
}

TEST(SchemaGrabberJson, InfersTopLevelArray) {
    grabber::SchemaGrabberOptions options;
    const auto schema = grabber::infer_json_schema(
        R"([{"id": 1}, {"id": 2}])", options, Redactor());
    ASSERT_TRUE(schema.has_value());
    EXPECT_EQ(schema->type, "array");
    ASSERT_EQ(schema->properties.size(), 1u);
    EXPECT_EQ(schema->properties[0].name, "id");
}

TEST(SchemaGrabberJson, RedactsSensitiveKeys) {
    grabber::SchemaGrabberOptions options;
    const auto schema = grabber::infer_json_schema(
        R"({"password": "hunter2", "id": 1})", options, Redactor());
    ASSERT_TRUE(schema.has_value());
    EXPECT_EQ(schema->properties[0].example, "1");
    EXPECT_EQ(schema->properties[1].example, "[REDACTED]");
    EXPECT_TRUE(schema->properties[1].sensitive);
}

TEST(SchemaGrabberJson, RejectsNonJson) {
    grabber::SchemaGrabberOptions options;
    EXPECT_FALSE(
        grabber::infer_json_schema("<html>hi</html>", options, Redactor()).has_value());
    EXPECT_FALSE(
        grabber::infer_json_schema("", options, Redactor()).has_value());
    EXPECT_FALSE(
        grabber::infer_json_schema("{broken", options, Redactor()).has_value());
}

TEST(SchemaGrabberForms, MatchesPostFormFields) {
    auto document = prowsetk::parse_html(
        "<form action='/api/users' method='POST'>"
        "<input name='name' value='ada'>"
        "<input name='age' type='number' required>"
        "<input name='admin' type='checkbox'>"
        "</form>",
        "https://example.test/app", "https://example.test/app");
    auto ep = endpoint("/api/users", "post");
    const auto fields =
        grabber::form_fields_for(*document, ep, {}, Redactor());
    ASSERT_EQ(fields.size(), 3u);
    EXPECT_EQ(fields[0].name, "admin");
    EXPECT_EQ(fields[0].type, "boolean");
    EXPECT_EQ(fields[1].name, "age");
    EXPECT_EQ(fields[1].type, "number");
    EXPECT_TRUE(fields[1].required);
    EXPECT_EQ(fields[2].name, "name");
    EXPECT_EQ(fields[2].example, "ada");
}

TEST(SchemaGrabberForms, IgnoresMethodMismatch) {
    auto document = prowsetk::parse_html(
        "<form action='/api/users' method='GET'><input name='q'></form>",
        "https://example.test/app", "https://example.test/app");
    auto ep = endpoint("/api/users", "post");
    EXPECT_TRUE(
        grabber::form_fields_for(*document, ep, {}, Redactor()).empty());
}

TEST(SchemaGrabberRequest, FormFieldsWinForPost) {
    auto document = prowsetk::parse_html(
        "<form action='/api/users' method='POST'><input name='name'></form>",
        "https://example.test/app", "https://example.test/app");
    auto ep = endpoint("/api/users", "post");
    ep.request_content_type = "application/x-www-form-urlencoded";
    std::string provenance;
    const auto schema = grabber::request_schema_for(ep, document.get(), {},
                                                    Redactor(), &provenance);
    ASSERT_TRUE(schema.has_value());
    EXPECT_EQ(provenance, "form-fields");
    ASSERT_EQ(schema->properties.size(), 1u);
    EXPECT_EQ(schema->properties[0].name, "name");
}

TEST(SchemaGrabberRequest, GetHasNoBodySchema) {
    auto ep = endpoint("/api/users", "get");
    EXPECT_FALSE(
        grabber::request_schema_for(ep, nullptr, {}, Redactor()).has_value());
}

TEST(SchemaGrabberRequest, ContentTypeHintFallsBack) {
    auto ep = endpoint("/api/users", "post");
    ep.request_content_type = "application/json";
    const auto schema =
        grabber::request_schema_for(ep, nullptr, {}, Redactor());
    ASSERT_TRUE(schema.has_value());
    EXPECT_EQ(schema->provenance, "content-type-hint");
}

TEST(SchemaGrabberDocument, EnrichesQueryParamsInYaml) {
    const auto result =
        grab_html("<script>fetch('/api/users?page=2&q=hi')</script>");
    ASSERT_EQ(result.schemas.size(), 1u);
    ASSERT_EQ(result.schemas[0].query.size(), 2u);
    EXPECT_NE(result.openapi_yaml.find("name: 'page'"), std::string::npos);
    EXPECT_NE(result.openapi_yaml.find("type: integer"), std::string::npos);
    EXPECT_NE(result.openapi_yaml.find("x-prowsetk-schema:"), std::string::npos);
    EXPECT_NE(result.openapi_yaml.find("x-prowsetk-plugin: schema-grabber"),
              std::string::npos);
}

TEST(SchemaGrabberDocument, EnrichesPostFormRequestBody) {
    const auto result = grab_html(
        "<form action='/api/users' method='POST'>"
        "<input name='name' value='ada'></form>");
    ASSERT_EQ(result.schemas.size(), 1u);
    ASSERT_TRUE(result.schemas[0].request_schema.has_value());
    EXPECT_EQ(result.schemas[0].request_provenance, "form-fields");
    EXPECT_NE(result.openapi_yaml.find("requestBody:"), std::string::npos);
    EXPECT_NE(result.openapi_yaml.find("'name':"), std::string::npos);
    // Postman urlencoded body carries the inferred field.
    EXPECT_NE(result.postman_json.find("urlencoded"), std::string::npos);
    EXPECT_NE(result.postman_json.find("\"name\""), std::string::npos);
}

TEST(SchemaGrabberDocument, TemplatizesNumericPathSegments) {
    const auto result =
        grab_html("<script>fetch('/api/users/123')</script>");
    ASSERT_EQ(result.schemas.size(), 1u);
    EXPECT_EQ(result.schemas[0].path_template, "/api/users/{id}");
    ASSERT_EQ(result.schemas[0].path_params.size(), 1u);
    EXPECT_NE(result.openapi_yaml.find("'/api/users/{id}':"), std::string::npos);
    EXPECT_NE(result.openapi_yaml.find("in: path"), std::string::npos);
    EXPECT_NE(result.postman_json.find(":id"), std::string::npos);
}

TEST(SchemaGrabberDocument, RedactsSecretsByDefault) {
    const auto result = grab_html(
        "<script>fetch('/api/users?api_key=supersecret')</script>");
    EXPECT_EQ(result.openapi_yaml.find("supersecret"), std::string::npos);
    EXPECT_EQ(result.postman_json.find("supersecret"), std::string::npos);
}

TEST(SchemaGrabberDocument, FiltersOrdinaryLinks) {
    const auto result = grab_html("<a href='/home'>Home</a>");
    EXPECT_TRUE(result.schemas.empty());
    EXPECT_NE(result.openapi_yaml.find("paths: {}"), std::string::npos);
}

TEST(SchemaGrabberDocument, ProducesDeterministicYaml) {
    const auto first =
        grab_html("<script>fetch('/api/users?page=1')</script>");
    const auto second =
        grab_html("<script>fetch('/api/users?page=1')</script>");
    EXPECT_EQ(first.openapi_yaml, second.openapi_yaml);
    EXPECT_EQ(first.postman_json, second.postman_json);
}

TEST(SchemaGrabberBodies, ResolvedBodiesYieldResponseSchemas) {
    auto document = prowsetk::parse_html(
        "<script>fetch('/api/users')</script>", "https://example.test/app",
        "https://example.test/app");
    auto ep = endpoint("/api/users", "get");
    grabber::ResolvedBody body;
    body.endpoint = ep;
    body.status = 200;
    body.body = R"({"id": 1, "name": "ada"})";
    body.content_type = "application/json";
    const auto enriched = grabber::enrich_endpoints(
        {ep}, {body}, document.get(), {}, Redactor());
    ASSERT_EQ(enriched.size(), 1u);
    ASSERT_EQ(enriched[0].responses.size(), 1u);
    ASSERT_TRUE(enriched[0].responses[0].schema.has_value());
    EXPECT_EQ(enriched[0].responses[0].schema->properties.size(), 2u);
    const auto yaml =
        grabber::render_schema_yaml(enriched, {}, Redactor());
    EXPECT_NE(yaml.find("Observed response"), std::string::npos);
    EXPECT_NE(yaml.find("'id':"), std::string::npos);
}

TEST(SchemaGrabberSession, ReportsMissingDocument) {
    Browser browser;
    auto session = browser.create_session();
    const auto result = grabber::grab_from_session(*session, {});
    ASSERT_EQ(result.warnings.size(), 1u);
    EXPECT_TRUE(result.schemas.empty());
}

TEST(SchemaGrabberSession, ProbesGetResponsesThroughNetwork) {
    Browser browser;
    auto network = std::make_unique<MemoryNetworkClient>();
    network->set_response("https://example.test/api/users?page=2",
                          response(200, R"({"id": 7, "name": "ada"})"));
    browser.set_network_client(std::move(network));
    auto session = browser.create_session();
    session->load_html("<script>fetch('/api/users?page=2')</script>",
                       "https://example.test/app");
    const auto result = grabber::grab_from_session(*session, {});
    ASSERT_EQ(result.schemas.size(), 1u);
    EXPECT_EQ(result.probe_count, 1u);
    ASSERT_EQ(result.schemas[0].responses.size(), 1u);
    ASSERT_TRUE(result.schemas[0].responses[0].schema.has_value());
    EXPECT_NE(result.openapi_yaml.find("'name':"), std::string::npos);
}

TEST(SchemaGrabberSession, NeverProbesPostEndpoints) {
    BrowserConfig config;
    config.javascript = false;
    Browser browser(config);
    auto network = std::make_unique<MemoryNetworkClient>();
    auto* observed = network.get();
    browser.set_network_client(std::move(network));
    auto session = browser.create_session();
    session->load_html(
        "<form action='/api/users' method='POST'><input name='name'></form>",
        "https://example.test/app");
    const auto before = observed->requests().size();
    const auto result = grabber::grab_from_session(*session, {});
    EXPECT_EQ(observed->requests().size(), before);
    EXPECT_EQ(result.probe_count, 0u);
    ASSERT_EQ(result.schemas.size(), 1u);
    ASSERT_TRUE(result.schemas[0].request_schema.has_value());
}

TEST(SchemaGrabberSession, HonorsProbeBudget) {
    Browser browser;
    auto network = std::make_unique<MemoryNetworkClient>();
    network->set_response("https://example.test/api/one", response(200, "{}"));
    network->set_response("https://example.test/api/two", response(200, "{}"));
    auto* observed = network.get();
    browser.set_network_client(std::move(network));
    auto session = browser.create_session();
    session->load_html("<script>fetch('/api/one'); fetch('/api/two')</script>",
                       "https://example.test/app");
    const auto page_requests = observed->requests().size();
    grabber::SchemaGrabberOptions options;
    options.max_probe_requests = 1;
    const auto result = grabber::grab_from_session(*session, options);
    EXPECT_EQ(observed->requests().size() - page_requests, 1u);
    EXPECT_FALSE(result.warnings.empty());
}

TEST(SchemaGrabberSession, RejectsCrossOriginProbesByDefault) {
    Browser browser;
    auto network = std::make_unique<MemoryNetworkClient>();
    network->set_response("https://other.test/api/items", response(200, "{}"));
    auto* observed = network.get();
    browser.set_network_client(std::move(network));
    auto session = browser.create_session();
    session->load_html("<a href='https://other.test/api/items'>x</a>",
                       "https://example.test/app");
    grabber::SchemaGrabberOptions options;
    options.minimum_confidence = 0.30;
    options.require_api_pattern = false;
    const auto page_requests = observed->requests().size();
    const auto result = grabber::grab_from_session(*session, options);
    // No cross-origin probe may be issued.
    EXPECT_EQ(observed->requests().size(), page_requests);
    EXPECT_EQ(result.probe_count, 0u);
}

TEST(SchemaGrabberPluginAbi, ExposesEntrySymbol) {
    void* handle = dlopen(SCHEMA_GRABBER_PLUGIN_PATH, RTLD_NOW);
    ASSERT_NE(handle, nullptr) << dlerror();
    using Entry = const void* (*)();
    auto entry = reinterpret_cast<Entry>(dlsym(handle, "prowsetk_plugin_entry"));
    ASSERT_NE(entry, nullptr);
    EXPECT_NE(entry(), nullptr);
    dlclose(handle);
}

#ifdef SCHEMA_GRABBER_HAVE_SCRAPE_ENDPOINTS
#include "prowsetk/plugins/scrape_endpoints.hpp"

TEST(SchemaGrabberComposition, EnrichesScrapeEndpointsOutput) {
    // End-to-end alongside scrape-endpoints: scrape for discovery, then
    // enrich the same endpoints so OpenAPI and Postman specs carry typed
    // URL parameters plus request and response schemas.
    namespace scrape = prowsetk::plugins::scrape_endpoints;
    Browser browser;
    auto session = browser.create_session();
    scrape::ScrapeEndpointsOptions scrape_options;
    scrape_options.html =
        "<script>fetch('/api/users/123?page=2')</script>"
        "<form action='/api/users' method='POST'>"
        "<input name='name' value='ada'></form>";
    scrape_options.base_url = "https://example.test/app";
    scrape_options.spa_probe = false;
    const auto scraped = scrape::scrape(*session, scrape_options);
    ASSERT_GE(scraped.resolved.size(), 2u);

    // Bodies resolved alongside scrape-endpoints feed response inference
    // without re-probing: the GET response carries an observed JSON body.
    std::vector<grabber::ResolvedBody> bodies;
    for (const auto& r : scraped.resolved) {
        grabber::ResolvedBody body;
        body.endpoint = r.endpoint;
        body.final_url = r.final_url;
        if (r.endpoint.method == "get") {
            body.status = 200;
            body.body = R"({"id": 123, "name": "ada"})";
            body.content_type = "application/json";
        }
        bodies.push_back(std::move(body));
    }
    auto document = session->document();
    ASSERT_NE(document, nullptr);
    const auto schemas = grabber::enrich_endpoints(
        scraped.endpoints, bodies, document.get(), {}, Redactor());
    ASSERT_EQ(schemas.size(), 2u);

    const grabber::EndpointSchema* get_schema = nullptr;
    const grabber::EndpointSchema* post_schema = nullptr;
    for (const auto& s : schemas) {
        if (s.endpoint.method == "get") get_schema = &s;
        if (s.endpoint.method == "post") post_schema = &s;
    }
    ASSERT_NE(get_schema, nullptr);
    ASSERT_NE(post_schema, nullptr);
    // URL parameters: templated path id plus typed query param.
    EXPECT_EQ(get_schema->path_template, "/api/users/{id}");
    ASSERT_FALSE(get_schema->query.empty());
    EXPECT_EQ(get_schema->query[0].name, "page");
    EXPECT_EQ(get_schema->query[0].type, "integer");
    // Response schema reverse-engineered from the resolved body.
    ASSERT_TRUE(get_schema->responses[0].schema.has_value());
    // POST request schema reverse-engineered from the form.
    ASSERT_TRUE(post_schema->request_schema.has_value());
    EXPECT_EQ(post_schema->request_provenance, "form-fields");

    const auto yaml = grabber::render_schema_yaml(schemas, {}, Redactor());
    EXPECT_NE(yaml.find("'/api/users/{id}':"), std::string::npos);
    EXPECT_NE(yaml.find("in: path"), std::string::npos);
    EXPECT_NE(yaml.find("in: query"), std::string::npos);
    EXPECT_NE(yaml.find("requestBody:"), std::string::npos);
    EXPECT_NE(yaml.find("Observed response"), std::string::npos);
    const auto postman =
        grabber::render_schema_postman_json(schemas, {}, Redactor());
    EXPECT_NE(postman.find(":id"), std::string::npos);
    EXPECT_NE(postman.find("urlencoded"), std::string::npos);
}
#endif
