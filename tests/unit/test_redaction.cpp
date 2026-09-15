#include <gtest/gtest.h>

#include "prowsetk/redaction.hpp"

using prowsetk::RedactionPolicy;
using prowsetk::Redactor;

TEST(Redaction, DefaultsCoverCommonSecrets) {
    const Redactor redactor;
    EXPECT_TRUE(redactor.is_sensitive_header("Authorization"));
    EXPECT_TRUE(redactor.is_sensitive_header("COOKIE"));
    EXPECT_TRUE(redactor.is_sensitive_query_parameter("access_token"));
    EXPECT_FALSE(redactor.is_sensitive_header("Accept"));
    EXPECT_FALSE(redactor.is_sensitive_query_parameter("page"));
}

TEST(Redaction, RedactsHeaderValues) {
    const Redactor redactor;
    EXPECT_EQ(redactor.redact_header("Authorization", "Bearer abc"),
              "[REDACTED]");
    EXPECT_EQ(redactor.redact_header("Accept", "text/html"), "text/html");
}

TEST(Redaction, RedactsUrlQueryParameters) {
    const Redactor redactor;
    const std::string redacted =
        redactor.redact_url("https://api.test/x?token=abc&page=2&api_key=zzz");
    EXPECT_NE(redacted.find("token=[REDACTED]"), std::string::npos);
    EXPECT_NE(redacted.find("page=2"), std::string::npos);
    EXPECT_EQ(redacted.find("abc"), std::string::npos);
    EXPECT_EQ(redacted.find("zzz"), std::string::npos);
}

TEST(Redaction, RedactsHeaderList) {
    const Redactor redactor;
    std::vector<std::pair<std::string, std::string>> headers = {
        {"Authorization", "Bearer abc"}, {"Accept", "application/json"}};
    const auto redacted = redactor.redact_headers(headers);
    ASSERT_EQ(redacted.size(), 2u);
    EXPECT_EQ(redacted[0].second, "[REDACTED]");
    EXPECT_EQ(redacted[1].second, "application/json");
}

TEST(Redaction, CustomPolicy) {
    RedactionPolicy policy;
    policy.header_names = {"x-custom"};
    policy.query_parameter_names = {"q"};
    policy.replacement = "***";
    const Redactor redactor(policy);
    EXPECT_EQ(redactor.redact_header("X-Custom", "value"), "***");
    EXPECT_EQ(redactor.redact_header("Authorization", "value"), "value");
    EXPECT_NE(redactor.redact_url("https://t/?q=1").find("q=***"),
              std::string::npos);
}
