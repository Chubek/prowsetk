#include <gtest/gtest.h>

#include <chrono>
#include <string>

#include "prowsetk/javascript_runtime.hpp"

namespace {

TEST(JavaScriptRuntime, EvaluatesScriptsAndPreservesPageContext) {
    auto runtime = prowsetk::make_javascript_runtime();
    if (runtime->name() == "null") GTEST_SKIP();
    EXPECT_EQ(runtime->evaluate("globalThis.answer = 40 + 2").value, "42");
    const auto result = runtime->evaluate("answer + 1");
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(result.value, "43");
}

TEST(JavaScriptRuntime, InstallsStringGlobalsWithoutCodeInjection) {
    auto runtime = prowsetk::make_javascript_runtime();
    if (runtime->name() == "null") GTEST_SKIP();
    runtime->set_global("payload", "'; throw new Error('injected'); //");
    const auto result = runtime->evaluate("payload");
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(result.value, "'; throw new Error('injected'); //");
}

TEST(JavaScriptRuntime, ReportsExceptionsWithUsefulContext) {
    auto runtime = prowsetk::make_javascript_runtime();
    if (runtime->name() == "null") GTEST_SKIP();
    const auto result = runtime->evaluate("throw new TypeError('broken')");
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("TypeError: broken"), std::string::npos);
    EXPECT_NE(result.error.find("<prowsetk>"), std::string::npos);
}

TEST(JavaScriptRuntime, EnforcesExecutionDeadline) {
    auto runtime = prowsetk::make_javascript_runtime();
    if (runtime->name() == "null") GTEST_SKIP();
    prowsetk::ScriptOptions options;
    options.timeout_ms = 20;
    const auto started = std::chrono::steady_clock::now();
    const auto result = runtime->evaluate("for (;;) {}", options);
    EXPECT_FALSE(result.ok);
    EXPECT_LT(std::chrono::steady_clock::now() - started,
              std::chrono::seconds(2));
}

TEST(JavaScriptRuntime, RejectsInvalidResourceLimits) {
    auto runtime = prowsetk::make_javascript_runtime();
    prowsetk::ScriptOptions options;
    options.timeout_ms = 0;
    EXPECT_FALSE(runtime->evaluate("1", options).ok);
    options.timeout_ms = 100;
    options.memory_limit_bytes = 0;
    EXPECT_FALSE(runtime->evaluate("1", options).ok);
}

TEST(JavaScriptRuntime, AdvertisesOnlyInstalledHostBindings) {
    auto runtime = prowsetk::make_javascript_runtime();
    const auto capabilities = runtime->capabilities();
    if (runtime->name() == "null") {
        EXPECT_FALSE(capabilities.has("javascript"));
        return;
    }
    EXPECT_TRUE(capabilities.has("javascript"));
    EXPECT_FALSE(capabilities.has("console"));
    EXPECT_FALSE(capabilities.has("fetch"));
}

}  // namespace
