#include <gtest/gtest.h>

#include <chrono>
#include <stdexcept>
#include <string>
#include <vector>

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

TEST(JavaScriptRuntime, AdvertisesInstalledWebPlatformBindings) {
    auto runtime = prowsetk::make_javascript_runtime();
    const auto capabilities = runtime->capabilities();
    if (runtime->name() == "null") {
        EXPECT_FALSE(capabilities.has("javascript"));
        return;
    }
    EXPECT_TRUE(capabilities.has("javascript"));
    EXPECT_TRUE(capabilities.has("console"));
    // The web platform shim installs these page APIs over host-mediated
    // primitives.
    for (const char* api : {"dom", "eventtarget", "xmlhttprequest", "fetch",
                            "timers", "storage", "url", "location",
                            "navigator"}) {
        EXPECT_TRUE(capabilities.has(api)) << api;
    }
}

TEST(JavaScriptRuntime, ForwardsConsoleCallsToHandler) {
    auto runtime = prowsetk::make_javascript_runtime();
    if (runtime->name() == "null") GTEST_SKIP();
    std::vector<prowsetk::ConsoleMessage> messages;
    runtime->set_console_handler(
        [&messages](const prowsetk::ConsoleMessage& message) {
            messages.push_back(message);
        });
    const auto result = runtime->evaluate(
        "console.log('hello', 42); console.warn('careful');");
    ASSERT_TRUE(result.ok) << result.error;
    ASSERT_EQ(messages.size(), 2u);
    EXPECT_EQ(messages[0].level, "log");
    EXPECT_EQ(messages[0].text, "hello 42");
    EXPECT_EQ(messages[1].level, "warn");
    EXPECT_EQ(messages[1].text, "careful");
}

TEST(JavaScriptRuntime, RunsPromiseReactionBeforeReturningToHost) {
    auto runtime = prowsetk::make_javascript_runtime();
    if (runtime->name() == "null") GTEST_SKIP();
    ASSERT_TRUE(runtime->evaluate(
        "globalThis.reacted = false; Promise.resolve().then(() => reacted = true);").ok);
    EXPECT_EQ(runtime->evaluate("reacted").value, "true");
}

TEST(JavaScriptRuntime, PromiseRejectionIsCaught) {
    auto runtime = prowsetk::make_javascript_runtime();
    if (runtime->name() == "null") GTEST_SKIP();
    const auto result = runtime->evaluate(
        "Promise.reject(new Error('rejected')).catch(e => e.message)");
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(result.value, "[object Promise]");
}

TEST(JavaScriptRuntime, AsyncAwaitWorks) {
    auto runtime = prowsetk::make_javascript_runtime();
    if (runtime->name() == "null") GTEST_SKIP();
    const auto result = runtime->evaluate(
        "async function foo() { return 42; } foo().then(x => x + 1)");
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(result.value, "[object Promise]");
}

TEST(JavaScriptRuntime, MicrotaskCheckpointLimitsJobs) {
    auto runtime = prowsetk::make_javascript_runtime();
    if (runtime->name() == "null") GTEST_SKIP();
    prowsetk::ScriptOptions options;
    options.max_microtask_jobs = 3;
    const auto result = runtime->evaluate(
        "let count = 0; for (let i = 0; i < 10; i++) queueMicrotask(() => count++); count",
        options);
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("microtask job limit exceeded"), std::string::npos);
}

TEST(JavaScriptRuntime, ArrayAndObjectLiterals) {
    auto runtime = prowsetk::make_javascript_runtime();
    if (runtime->name() == "null") GTEST_SKIP();
    EXPECT_EQ(runtime->evaluate("[1,2,3].length").value, "3");
    EXPECT_EQ(runtime->evaluate("({a:1,b:2}).a").value, "1");
}

TEST(JavaScriptRuntime, GlobalThisIsSharedAcrossEvaluations) {
    auto runtime = prowsetk::make_javascript_runtime();
    if (runtime->name() == "null") GTEST_SKIP();
    runtime->evaluate("globalThis.shared = 'value'");
    const auto result = runtime->evaluate("shared");
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(result.value, "value");
}

TEST(JavaScriptRuntime, NaNAndInfinitySerialization) {
    auto runtime = prowsetk::make_javascript_runtime();
    if (runtime->name() == "null") GTEST_SKIP();
    EXPECT_EQ(runtime->evaluate("NaN").value, "NaN");
    EXPECT_EQ(runtime->evaluate("Infinity").value, "Infinity");
    EXPECT_EQ(runtime->evaluate("-Infinity").value, "-Infinity");
}

TEST(JavaScriptRuntime, RegexAndDateSerialization) {
    auto runtime = prowsetk::make_javascript_runtime();
    if (runtime->name() == "null") GTEST_SKIP();
    const auto regex = runtime->evaluate("/abc/g");
    ASSERT_TRUE(regex.ok) << regex.error;
    const auto date = runtime->evaluate("new Date(0)");
    ASSERT_TRUE(date.ok) << date.error;
}

TEST(JavaScriptRuntime, StrictModeWorks) {
    auto runtime = prowsetk::make_javascript_runtime();
    if (runtime->name() == "null") GTEST_SKIP();
    const auto result = runtime->evaluate("'use strict'; x = 1");
    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(result.error.empty());
}

TEST(JavaScriptRuntime, ConsoleHandlerMultipleCalls) {
    auto runtime = prowsetk::make_javascript_runtime();
    if (runtime->name() == "null") GTEST_SKIP();
    std::vector<prowsetk::ConsoleMessage> messages;
    runtime->set_console_handler(
        [&messages](const prowsetk::ConsoleMessage& message) {
            messages.push_back(message);
        });
    runtime->evaluate("console.log('one'); console.log('two'); console.log('three')");
    ASSERT_EQ(messages.size(), 3u);
    EXPECT_EQ(messages[0].text, "one");
    EXPECT_EQ(messages[1].text, "two");
    EXPECT_EQ(messages[2].text, "three");
}

TEST(JavaScriptRuntime, ConsoleHandlerDifferentLevels) {
    auto runtime = prowsetk::make_javascript_runtime();
    if (runtime->name() == "null") GTEST_SKIP();
    std::vector<prowsetk::ConsoleMessage> messages;
    runtime->set_console_handler(
        [&messages](const prowsetk::ConsoleMessage& message) {
            messages.push_back(message);
        });
    runtime->evaluate("console.debug('d'); console.info('i'); console.error('e')");
    ASSERT_EQ(messages.size(), 3u);
    EXPECT_EQ(messages[0].level, "debug");
    EXPECT_EQ(messages[1].level, "info");
    EXPECT_EQ(messages[2].level, "error");
}

TEST(JavaScriptRuntime, ErrorObjectStackTrace) {
    auto runtime = prowsetk::make_javascript_runtime();
    if (runtime->name() == "null") GTEST_SKIP();
    const auto result = runtime->evaluate(
        "try { throw new Error('test'); } catch (e) { e.message }");
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(result.value, "test");
}

TEST(JavaScriptRuntime, SetGlobalOverwritesExisting) {
    auto runtime = prowsetk::make_javascript_runtime();
    if (runtime->name() == "null") GTEST_SKIP();
    runtime->set_global("name", "first");
    runtime->set_global("name", "second");
    EXPECT_EQ(runtime->evaluate("name").value, "second");
}

}  // namespace
