#include <gtest/gtest.h>

#include <algorithm>

#include "prowsetk/document.hpp"
#include "prowsetk/endpoint_extraction.hpp"

using prowsetk::EndpointExtractionOptions;
using prowsetk::EndpointExtractor;
using prowsetk::parse_html;

namespace {
const char* kHtml = R"HTML(<html><head><title>API demo</title></head><body>
  <a href="/api/users">users</a>
  <a href="/about">about</a>
  <form action="/api/login" method="POST">
    <input name="username">
    <input type="password" name="password">
  </form>
  <script>
    fetch('/api/items?token=supersecret&page=1');
    var xhr = new XMLHttpRequest();
    xhr.open('DELETE', '/api/items/1');
  </script>
</body></html>)HTML";
}

TEST(EndpointExtraction, DiscoversFormsLinksAndScripts) {
    const auto document = parse_html(kHtml, "https://example.com/");
    EndpointExtractor extractor;
    const auto result = extractor.extract(*document);

    bool found_login = false;
    bool found_items = false;
    bool found_delete = false;
    for (const auto& endpoint : result.endpoints) {
        if (endpoint.path == "/api/login") {
            found_login = true;
            EXPECT_EQ(endpoint.method, "post");
            EXPECT_EQ(endpoint.discovery_method, "html-form");
        }
        if (endpoint.path == "/api/items") {
            found_items = true;
            EXPECT_EQ(endpoint.discovery_method, "inline-script");
        }
        if (endpoint.path == "/api/items/1") {
            found_delete = true;
            EXPECT_EQ(endpoint.method, "delete");
        }
    }
    EXPECT_TRUE(found_login);
    EXPECT_TRUE(found_items);
    EXPECT_TRUE(found_delete);
}

TEST(EndpointExtraction, IgnoresNonApiLinks) {
    const auto document = parse_html(kHtml, "https://example.com/");
    EndpointExtractor extractor;
    const auto result = extractor.extract(*document);
    for (const auto& endpoint : result.endpoints) {
        EXPECT_NE(endpoint.path, "/about");
    }
}

TEST(EndpointExtraction, GeneratesOpenApiWithProvenance) {
    const auto document = parse_html(kHtml, "https://example.com/");
    EndpointExtractor extractor;
    const auto result = extractor.extract(*document);

    EXPECT_NE(result.openapi_yaml.find("openapi: 3.1.0"), std::string::npos);
    EXPECT_NE(result.openapi_yaml.find("x-prowsetk-provenance"),
              std::string::npos);
    EXPECT_NE(result.openapi_yaml.find("confidence:"), std::string::npos);
    EXPECT_NE(result.openapi_yaml.find("inferred: true"), std::string::npos);
}

TEST(EndpointExtraction, RedactsSecretsByDefault) {
    const auto document = parse_html(kHtml, "https://example.com/");
    EndpointExtractor extractor;
    const auto result = extractor.extract(*document);
    EXPECT_EQ(result.openapi_yaml.find("supersecret"), std::string::npos);
    EXPECT_NE(result.openapi_yaml.find("REDACTED"), std::string::npos);
}

TEST(EndpointExtraction, ObservedNetworkEndpointsHaveHighConfidence) {
    EndpointExtractor extractor;
    extractor.observe("POST", "https://example.com/api/orders?api_key=zzz", 201,
                      "application/json");
    const auto document = parse_html("<html></html>", "https://example.com/");
    const auto result = extractor.extract(*document);
    ASSERT_EQ(result.endpoints.size(), 1u);
    EXPECT_DOUBLE_EQ(result.endpoints[0].confidence, 0.95);
    EXPECT_EQ(result.endpoints[0].method, "post");
    EXPECT_EQ(result.endpoints[0].response_content_type, "application/json");
}

TEST(EndpointExtraction, ObservedNetworkCanBeDisabled) {
    EndpointExtractionOptions options;
    options.observe_network = false;
    EndpointExtractor extractor(options);
    extractor.observe("POST", "https://example.com/api/orders", 201,
                      "application/json");

    const auto document = parse_html("<html></html>", "https://example.com/");
    const auto result = extractor.extract(*document);

    EXPECT_TRUE(result.endpoints.empty());
}

TEST(EndpointExtraction, FetchOptionsInferHttpMethodAndQueryParameters) {
    const auto document = parse_html(R"HTML(
        <script>
          fetch('/api/orders?customer=42', { method: 'PATCH' });
          fetch("/api/search?q=term");
        </script>
    )HTML",
                                     "https://example.com/");

    EndpointExtractor extractor;
    const auto result = extractor.extract(*document);

    bool found_patch = false;
    bool found_get = false;
    for (const auto& endpoint : result.endpoints) {
        if (endpoint.path == "/api/orders") {
            found_patch = true;
            EXPECT_EQ(endpoint.method, "patch");
            EXPECT_NE(std::find(endpoint.parameters.begin(),
                                endpoint.parameters.end(), "customer"),
                      endpoint.parameters.end());
        }
        if (endpoint.path == "/api/search") {
            found_get = true;
            EXPECT_EQ(endpoint.method, "get");
            EXPECT_NE(std::find(endpoint.parameters.begin(),
                                endpoint.parameters.end(), "q"),
                      endpoint.parameters.end());
        }
    }
    EXPECT_TRUE(found_patch);
    EXPECT_TRUE(found_get);
}

TEST(EndpointExtraction, MergesDuplicateDiscoveriesByMethodAndPath) {
    EndpointExtractor extractor;
    extractor.observe("POST", "https://example.com/api/login?from=network", 200,
                      "application/json");
    const auto document = parse_html(R"HTML(
        <form action="/api/login?from=form" method="POST">
          <input name="username">
        </form>
    )HTML",
                                     "https://example.com/");

    const auto result = extractor.extract(*document);

    ASSERT_EQ(result.endpoints.size(), 1u);
    EXPECT_EQ(result.endpoints[0].path, "/api/login");
    EXPECT_EQ(result.endpoints[0].method, "post");
    EXPECT_DOUBLE_EQ(result.endpoints[0].confidence, 0.95);
    EXPECT_NE(std::find(result.endpoints[0].parameters.begin(),
                        result.endpoints[0].parameters.end(), "username"),
              result.endpoints[0].parameters.end());
    EXPECT_NE(std::find(result.endpoints[0].parameters.begin(),
                        result.endpoints[0].parameters.end(), "from"),
              result.endpoints[0].parameters.end());
}

TEST(EndpointExtraction, ConfidenceThresholdFiltersResults) {
    EndpointExtractionOptions options;
    options.minimum_confidence = 0.90;
    const auto document = parse_html(kHtml, "https://example.com/");
    EndpointExtractor extractor(options);
    const auto result = extractor.extract(*document);
    EXPECT_TRUE(result.endpoints.empty());
    EXPECT_FALSE(result.warnings.empty());
}
