#include <gtest/gtest.h>
#include "prowsetk/plugins/scrape2postman.hpp"
#include <fstream>
#include <limits>

namespace postman = prowsetk::plugins::scrape2postman;
namespace {
postman::ResolvedEndpoint endpoint(std::string url = "https://example.test/api/users",
                                   std::string method = "get") {
    postman::ResolvedEndpoint r;
    r.endpoint.url = std::move(url);
    r.endpoint.method = std::move(method);
    r.endpoint.path = "/api/users";
    r.endpoint.source = "https://example.test/app";
    r.endpoint.discovery_method = "script-fetch";
    r.endpoint.confidence = 0.75;
    return r;
}
}

TEST(Postman, EmptyCollectionHasRequiredFields) {
    const auto json = postman::render_postman_json({});
    EXPECT_NE(json.find("\"info\""), std::string::npos);
    EXPECT_NE(json.find("collection/v2.1.0/collection.json"), std::string::npos);
    EXPECT_NE(json.find("\"item\": [\n  ]"), std::string::npos);
}

TEST(Postman, EscapesJsonControlsAndUnicode) {
    postman::Scrape2PostmanOptions options;
    options.collection_name = std::string("A\"\\\n\t\r") + '\0' + " café";
    const auto json = postman::render_postman_json({}, options);
    EXPECT_NE(json.find("A\\\"\\\\\\u000a\\u0009\\u000d\\u0000 café"), std::string::npos);
}

TEST(Postman, PreservesMethodsHostsAndStableOrdering) {
    const auto a = endpoint();
    const auto b = endpoint("https://example.test/api/users", "post");
    const auto c = endpoint("https://other.test/api/users");
    const auto json = postman::render_postman_json({b, c, a, a});
    EXPECT_EQ(json, postman::render_postman_json({a, c, b}));
    EXPECT_NE(json.find("\"method\": \"POST\""), std::string::npos);
    EXPECT_NE(json.find("https://other.test/api/users"), std::string::npos);
}

TEST(Postman, RedactsEveryUrlAndOmitsCapturedBodiesNotesAndErrors) {
    auto r = endpoint("https://user:credential@example.test/api/users?%74oken=secret1&q=visible#secret2");
    r.endpoint.source = "https://example.test/?password=secret3";
    r.final_url = "https://example.test/api/final?access_token=secret4";
    r.redirect_chain = {"https://example.test/api/hop?api_key=secret5"};
    r.json_discovered = {"/api/next?client_secret=secret6"};
    r.body = "secret7";
    r.error = "secret8";
    r.endpoint.notes = {"secret9"};
    r.status = 201;
    const auto json = postman::render_postman_json({r});
    for (int i = 1; i <= 9; ++i)
        EXPECT_EQ(json.find("secret" + std::to_string(i)), std::string::npos);
    EXPECT_EQ(json.find("user:credential"), std::string::npos);
    EXPECT_NE(json.find("q=visible"), std::string::npos);
    EXPECT_NE(json.find("[REDACTED]"), std::string::npos);
    EXPECT_NE(json.find("Observed HTTP status: 201"), std::string::npos);
}

TEST(Postman, SupportsCustomRedactionAndExplicitOptOut) {
    auto policy = prowsetk::RedactionPolicy::defaults();
    policy.query_parameter_names.push_back("custom");
    auto r = endpoint("https://example.test/api/users?custom=private-value");
    EXPECT_EQ(postman::render_postman_json({r}, {}, prowsetk::Redactor(policy)).find("private-value"), std::string::npos);
    postman::Scrape2PostmanOptions options;
    options.redact_secrets = false;
    EXPECT_NE(postman::render_postman_json({r}, options).find("private-value"), std::string::npos);
}

TEST(Postman, DisablingProvenanceKeepsHeuristicConfidence) {
    postman::Scrape2PostmanOptions options;
    options.include_provenance = false;
    auto r = endpoint();
    r.endpoint.confidence = std::numeric_limits<double>::quiet_NaN();
    const auto json = postman::render_postman_json({r}, options);
    EXPECT_EQ(json.find("Source:"), std::string::npos);
    EXPECT_EQ(json.find("script-fetch"), std::string::npos);
    EXPECT_NE(json.find("Confidence: 0"), std::string::npos);
    EXPECT_NE(json.find("Heuristic discovery"), std::string::npos);
}

TEST(Postman, QueryPlaceholdersDoNotDuplicateObservedParameters) {
    auto r = endpoint("https://example.test/api/users?q=visible");
    r.endpoint.parameters = {"q", "page size", "page size"};
    const auto json = postman::render_postman_json({r});
    EXPECT_NE(json.find("q=visible&page%20size="), std::string::npos);
    EXPECT_EQ(json.find("q=visible&q="), std::string::npos);
}

TEST(Postman, FormBodyHasNamesButNoPrivateValues) {
    auto document = prowsetk::parse_html(
        "<form action='/api/login' method='post'><input name='username' value='private-user'>"
        "<input name='password' type='password' value='private-pass'></form>", "https://example.test/");
    const auto result = postman::scrape_from_document(*document);
    ASSERT_EQ(result.endpoints.size(), 1u);
    EXPECT_NE(result.postman_json.find("\"method\": \"POST\""), std::string::npos);
    EXPECT_NE(result.postman_json.find("\"mode\": \"urlencoded\""), std::string::npos);
    EXPECT_NE(result.postman_json.find("\"key\": \"password\""), std::string::npos);
    EXPECT_EQ(result.postman_json.find("private-"), std::string::npos);
}

TEST(Postman, DiscoveryFiltersAndConfidenceOptionsAreForwarded) {
    auto document = prowsetk::parse_html(
        "<a href='/home'>home</a><script>fetch('/api/users')</script>", "https://example.test/");
    auto result = postman::scrape_from_document(*document);
    ASSERT_EQ(result.endpoints.size(), 1u);
    EXPECT_EQ(result.endpoints.front().path, "/api/users");
    postman::Scrape2PostmanOptions options;
    options.inspect_scripts = false;
    EXPECT_TRUE(postman::scrape_from_document(*document, options).endpoints.empty());
    options.inspect_scripts = true;
    options.minimum_confidence = 1.0;
    EXPECT_TRUE(postman::scrape_from_document(*document, options).endpoints.empty());
}

TEST(Postman, WritesExactJsonAndReportsOutputErrors) {
    postman::Scrape2PostmanResult result;
    result.postman_json = postman::render_postman_json({endpoint()});
    const auto path = std::filesystem::path(TEST_BINARY_DIR) / "unit.postman_collection.json";
    result.write_postman_json(path);
    std::ifstream input(path, std::ios::binary);
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(input), {}), result.postman_json);
    EXPECT_THROW(result.write_postman_json(TEST_BINARY_DIR), std::ios_base::failure);
}
