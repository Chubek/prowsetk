#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "Flatwork-Module.h"
#include "prowsetk/error.hpp"
#include "prowsetk/flatworm_module.hpp"
#include "prowsetk/javascript_runtime.hpp"

namespace {

using prowsetk::Error;
using prowsetk::ErrorCode;
using prowsetk::FlatwormModule;
using prowsetk::FlatwormModuleRegistry;

FlatwormStatus echo(FlatwormCall* call) {
    if (call->argument_count == 0) return FLATWORM_STATUS_INVALID_ARGUMENT;
    return call->api->set_result(call, &call->arguments[0]);
}

FlatwormModuleDefinition definition(const char* name = "unit") {
    static const FlatwormFunction functions[] = {{"echo", 1, echo}};
    return {FLATWORM_MODULE_ABI_VERSION, sizeof(FlatwormModuleDefinition), name, "1.0", "unit",
            nullptr, nullptr, functions, 1, nullptr, 0};
}

void expect_error(const std::function<void()>& action, ErrorCode code) {
    try {
        action();
        FAIL() << "expected an Error";
    } catch (const Error& error) {
        EXPECT_EQ(error.code(), code) << error.what();
    }
}

TEST(FlatwormModuleDefinition, CopiesMetadataAndConstantBytes) {
    char name[] = "copied";
    char bytes[] = "hello";
    FlatwormConstant constant{"text", {FLATWORM_VALUE_STRING, 0, 0, {bytes, 5}}};
    auto source = definition(name);
    source.constants = &constant;
    source.constant_count = 1;
    auto module = FlatwormModule::from_static(source);
    name[0] = 'x';
    bytes[0] = 'x';
    EXPECT_EQ(module->info().name, "copied");
    EXPECT_EQ(module->info().exports, (std::vector<std::string>{"echo", "text"}));
    auto runtime = prowsetk::make_javascript_runtime();
    if (runtime->name() == "null") return;
    runtime->install_module(module);
    EXPECT_EQ(runtime->evaluate("Flatworm.module('copied').text").value, "hello");
}

TEST(FlatwormModuleDefinition, RejectsAbiSizeAndMalformedMetadata) {
    auto source = definition();
    source.abi_version++;
    expect_error([&] { FlatwormModule::from_static(source); }, ErrorCode::InvalidArgument);
    source = definition();
    source.struct_size = offsetof(FlatwormModuleDefinition, name);
    expect_error([&] { FlatwormModule::from_static(source); }, ErrorCode::InvalidArgument);
    for (const char* name : {"", "bad/name", "1bad", "x:y", "two words"}) {
        source = definition(name);
        expect_error([&] { FlatwormModule::from_static(source); }, ErrorCode::InvalidArgument);
    }
    std::string long_name(65, 'x');
    source = definition(long_name.c_str());
    expect_error([&] { FlatwormModule::from_static(source); }, ErrorCode::InvalidArgument);
    source = definition();
    source.version = nullptr;
    expect_error([&] { FlatwormModule::from_static(source); }, ErrorCode::InvalidArgument);
}

TEST(FlatwormModuleDefinition, ValidatesExportsAndAggregateBudgets) {
    auto source = definition();
    source.functions = nullptr;
    expect_error([&] { FlatwormModule::from_static(source); }, ErrorCode::InvalidArgument);
    source = definition();
    source.function_count = FLATWORM_MODULE_MAX_EXPORTS + 1;
    expect_error([&] { FlatwormModule::from_static(source); }, ErrorCode::ResourceLimit);
    FlatwormFunction bad{"echo", FLATWORM_MODULE_MAX_ARGUMENTS + 1, echo};
    source = definition();
    source.functions = &bad;
    expect_error([&] { FlatwormModule::from_static(source); }, ErrorCode::InvalidArgument);
    bad = {"bad-name", 0, echo};
    expect_error([&] { FlatwormModule::from_static(source); }, ErrorCode::InvalidArgument);
    bad = {"echo", 0, nullptr};
    expect_error([&] { FlatwormModule::from_static(source); }, ErrorCode::InvalidArgument);
    source = definition();
    FlatwormConstant constant{"echo", {}};
    source.constants = &constant;
    source.constant_count = 1;
    expect_error([&] { FlatwormModule::from_static(source); }, ErrorCode::InvalidArgument);
    constant.name = "constant";
    constant.value.type = 99u;
    expect_error([&] { FlatwormModule::from_static(source); }, ErrorCode::InvalidArgument);
    constant.value = {FLATWORM_VALUE_STRING, 0, 0, {nullptr, 1}};
    expect_error([&] { FlatwormModule::from_static(source); }, ErrorCode::InvalidArgument);
    constant.value.bytes = {"x", FLATWORM_MODULE_MAX_VALUE_BYTES + 1};
    expect_error([&] { FlatwormModule::from_static(source); }, ErrorCode::ResourceLimit);
    std::string bytes(FLATWORM_MODULE_MAX_VALUE_BYTES, 'x');
    FlatwormConstant constants[] = {
        {"one", {FLATWORM_VALUE_STRING, 0, 0, {bytes.data(), bytes.size()}}},
        {"two", {FLATWORM_VALUE_STRING, 0, 0, {"x", 1}}}
    };
    source.constants = constants;
    source.constant_count = 2;
    expect_error([&] { FlatwormModule::from_static(source); }, ErrorCode::ResourceLimit);
}

TEST(FlatwormModuleDefinition, RegistrySnapshotsRejectDuplicatesAndBoundModuleCount) {
    FlatwormModuleRegistry registry;
    expect_error([&] { registry.add(nullptr); }, ErrorCode::InvalidArgument);
    const auto module = registry.register_static(definition());
    const auto snapshot = registry.modules();
    ASSERT_EQ(snapshot.size(), 1u);
    EXPECT_EQ(snapshot[0], module);
    expect_error([&] { registry.add(module); }, ErrorCode::InvalidArgument);
    EXPECT_TRUE(registry.remove("unit"));
    EXPECT_FALSE(registry.remove("unit"));
    EXPECT_TRUE(registry.modules().empty());
    EXPECT_EQ(snapshot[0]->info().name, "unit");
    for (unsigned i = 0; i < FLATWORM_MODULE_MAX_MODULES; ++i) {
        const std::string name = "m" + std::to_string(i);
        registry.register_static(definition(name.c_str()));
    }
    expect_error([&] { registry.register_static(definition("overflow")); }, ErrorCode::ResourceLimit);
}

TEST(FlatwormModuleDefinition, LoadsRealCSharedLibraryAndRejectsInvalidLibraries) {
    FlatwormModuleRegistry registry;
    auto module = registry.load_native(TEST_MODULE_PATH);
    EXPECT_EQ(module->info().name, "fixture");
    EXPECT_EQ(module->info().version, "1.0.0");
    EXPECT_EQ(module->info().exports.size(), 6u);
    expect_error([] { FlatwormModule::load_native(TEST_BAD_MODULE_PATH); }, ErrorCode::InvalidArgument);
    expect_error([] { FlatwormModule::load_native(TEST_NO_ENTRY_PATH); }, ErrorCode::InvalidArgument);
    try {
        FlatwormModule::load_native("missing/host-private-marker.so");
        FAIL();
    } catch (const Error& error) {
        EXPECT_EQ(error.code(), ErrorCode::IoError);
        EXPECT_EQ(std::string(error.what()).find("host-private-marker"), std::string::npos);
    }
}

TEST(FlatwormModuleDefinition, NullRuntimeReportsUnsupported) {
    auto runtime = prowsetk::make_null_javascript_runtime();
    EXPECT_FALSE(runtime->capabilities().has("javascript-modules"));
    EXPECT_TRUE(runtime->modules().empty());
    EXPECT_FALSE(runtime->evaluate_module("export const x = 1").ok);
    expect_error([&] { runtime->install_module(FlatwormModule::from_static(definition())); },
                 ErrorCode::Unsupported);
}

TEST(FlatwormModuleDefinition, ResolvesCppEntryWithCLinkageAndContainsEntryExceptions) {
    try {
        FlatwormModule::load_native(TEST_THROWING_ENTRY_PATH);
        FAIL();
    } catch (const Error& error) {
        EXPECT_EQ(error.code(), ErrorCode::JavaScriptError);
        EXPECT_EQ(std::string(error.what()), "Flatworm module entry failed");
    }
}

class FlatwormModuleRuntime : public ::testing::Test {
protected:
    void SetUp() override {
        runtime = prowsetk::make_javascript_runtime();
        if (runtime->name() == "null") GTEST_SKIP();
        runtime->install_module(FlatwormModule::load_native(TEST_MODULE_PATH));
        const auto installed = runtime->evaluate("globalThis.m = Flatworm.module('fixture'); true");
        ASSERT_TRUE(installed.ok) << installed.error;
    }
    std::unique_ptr<prowsetk::JavaScriptRuntime> runtime;
};

TEST_F(FlatwormModuleRuntime, TransfersScalarsJsonAndEmbeddedNuls) {
    for (const std::string expression : {
            "m.echo(42) === 42", "m.echo(true) === true", "m.echo(null) === null",
            "m.echo(undefined) === undefined", "m.echo(NaN).toString() === 'NaN'",
            "m.echo(Infinity) === Infinity", "m.echo('é\\u0000z') === 'é\\u0000z'",
            "JSON.stringify(m.echo({a:[1,null,true],b:'str'})) === '{\"a\":[1,null,true],\"b\":\"str\"}'",
            "m.answer === 42 && m.ready && m.label === 'native' && m.schema.kind === 'fixture'",
            "m.echo.length === 1 && m.next.length === 0", "m.next() === 1 && m.next() === 2"}) {
        const auto result = runtime->evaluate(expression);
        ASSERT_TRUE(result.ok) << expression << ": " << result.error;
        EXPECT_EQ(result.value, "true") << expression;
    }
    EXPECT_TRUE(runtime->capabilities().has("javascript-modules"));
    ASSERT_EQ(runtime->modules().size(), 1u);
    EXPECT_EQ(runtime->modules()[0].name, "fixture");
}

TEST_F(FlatwormModuleRuntime, KeepsExportsReadOnlyAndDoesNotCoerceNames) {
    EXPECT_EQ(runtime->evaluate("Object.isFrozen(m) && Object.getPrototypeOf(m) === null && Object.isFrozen(Flatworm)").value, "true");
    for (const char* script : {"'use strict'; m.echo = 1", "'use strict'; m.extra = 1",
                               "'use strict'; Flatworm = {}", "Flatworm.module({toString(){throw 'coerced'}})",
                               "Flatworm.module('missing-private-marker')"}) {
        const auto result = runtime->evaluate(script);
        EXPECT_FALSE(result.ok);
        EXPECT_EQ(result.error.find("coerced"), std::string::npos);
        EXPECT_EQ(result.error.find("missing-private-marker"), std::string::npos);
    }
    expect_error([&] { runtime->install_module(FlatwormModule::load_native(TEST_MODULE_PATH)); },
                 ErrorCode::InvalidArgument);
    expect_error([&] { runtime->install_module(nullptr); }, ErrorCode::InvalidArgument);
    EXPECT_EQ(runtime->evaluate("m.echo(7)").value, "7");
}

TEST_F(FlatwormModuleRuntime, SupportsNativeEsImportsAndDynamicImports) {
    auto result = runtime->evaluate_module(
        "import {echo, next, answer} from 'flatworm:fixture'; globalThis.esValue = echo(answer) + next();");
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(runtime->evaluate("esValue").value, "43");
    result = runtime->evaluate_module(
        "import * as fixture from 'flatworm:fixture'; globalThis.secondValue = fixture.next();");
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(runtime->evaluate("secondValue").value, "2");
    result = runtime->evaluate(
        "import('flatworm:fixture').then(n => globalThis.dynamicValue = n.echo('dynamic'));");
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(runtime->evaluate("dynamicValue").value, "dynamic");
    result = runtime->evaluate_module("throw new Error('top-level failure');");
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("top-level failure"), std::string::npos);
}

TEST_F(FlatwormModuleRuntime, RejectsUnregisteredFileAndNetworkImports) {
    for (const char* source : {"import 'flatworm:unknown'", "import '/tmp/private-marker.js'",
                               "import './flatworm:fixture'", "import 'https://host.test/private-marker.js'"}) {
        const auto result = runtime->evaluate_module(source);
        EXPECT_FALSE(result.ok);
        EXPECT_EQ(result.error.find("private-marker"), std::string::npos);
    }
    EXPECT_EQ(runtime->evaluate("m.echo('still alive')").value, "still alive");
}

TEST_F(FlatwormModuleRuntime, EnforcesArgumentBytesCountAndCallDepth) {
    for (const char* expression : {
            "m.echo('x'.repeat(1048577))", "m.echo('x'.repeat(524289), 'y'.repeat(524289))",
            "m.echo(...Array(65).fill(0))"}) {
        const auto result = runtime->evaluate(expression);
        EXPECT_FALSE(result.ok) << expression;
        EXPECT_NE(result.error.find("limit exceeded"), std::string::npos) << result.error;
    }
    // Instrumented QuickJS may exhaust its stack before the native bridge's
    // depth bound. Both limits must reject recursive conversion and recover.
    const auto recursive = runtime->evaluate(
        "globalThis.nativeConversionDepth = 0; "
        "m.echo({toJSON(){++nativeConversionDepth; return m.echo(this)}})");
    EXPECT_FALSE(recursive.ok);
    EXPECT_NE(recursive.error.find("TypeError:"), std::string::npos) << recursive.error;
    const auto depth = runtime->evaluate("nativeConversionDepth");
    ASSERT_TRUE(depth.ok) << depth.error;
    EXPECT_GT(std::stoi(depth.value), 0);
    EXPECT_LE(static_cast<unsigned>(std::stoi(depth.value)), FLATWORM_MODULE_MAX_CALL_DEPTH);
    for (const char* expression : {"m.echo(() => 1)", "m.echo(Symbol('private-marker'))", "m.echo(1n)",
                                   "const cyclic = {}; cyclic.self = cyclic; m.echo(cyclic)"}) {
        const auto result = runtime->evaluate(expression);
        EXPECT_FALSE(result.ok);
        EXPECT_EQ(result.error.find("private-marker"), std::string::npos);
    }
    EXPECT_EQ(runtime->evaluate("m.echo('ok')").value, "ok");
}

TEST_F(FlatwormModuleRuntime, JsonConversionFailuresDoNotExposeGetterOrToJsonExceptions) {
    for (const char* expression : {
            "m.echo({toJSON(){throw new Error('private-conversion-marker')}})",
            "m.echo({get value(){throw 'private-conversion-marker'}})",
            "m.echo({toJSON(){throw {toString(){return 'private-conversion-marker'}}}})"}) {
        const auto result = runtime->evaluate(expression);
        EXPECT_FALSE(result.ok);
        EXPECT_NE(result.error.find("TypeError:"), std::string::npos) << result.error;
        EXPECT_EQ(result.error.find("private-conversion-marker"), std::string::npos);
    }
    EXPECT_EQ(runtime->evaluate("m.echo({ok:42}).ok").value, "42");
}

FlatwormStatus ignored_bad_result(FlatwormCall* call) {
    const FlatwormValue value{FLATWORM_VALUE_JSON, 0, 0, {"private-result-marker", 21}};
    (void)call->api->set_result(call, &value);
    return FLATWORM_STATUS_OK;
}
FlatwormStatus ignored_oversized_result(FlatwormCall* call) {
    const FlatwormValue value{FLATWORM_VALUE_STRING, 0, 0, {"x", FLATWORM_MODULE_MAX_VALUE_BYTES + 1}};
    (void)call->api->set_result(call, &value);
    return FLATWORM_STATUS_OK;
}
FlatwormStatus throwing_function(FlatwormCall*) { throw std::runtime_error("private-exception-marker"); }
FlatwormStatus failing_function(FlatwormCall*) { return FLATWORM_STATUS_ERROR; }
FlatwormStatus undefined_function(FlatwormCall*) { return FLATWORM_STATUS_OK; }
FlatwormStatus local_result(FlatwormCall* call) {
    std::string text = "temporary buffer";
    const FlatwormValue value{FLATWORM_VALUE_STRING, 0, 0, {text.data(), text.size()}};
    return call->api->set_result(call, &value);
}
FlatwormStatus unterminated_json(FlatwormCall* call) {
    const char bytes[] = {'{', '"', 'n', '"', ':', '1', '}'};
    const FlatwormValue value{FLATWORM_VALUE_JSON, 0, 0, {bytes, sizeof(bytes)}};
    return call->api->set_result(call, &value);
}

TEST_F(FlatwormModuleRuntime, CopiesResultsAndContainsNativeFailuresWithoutExposingValues) {
    const FlatwormFunction functions[] = {
        {"bad", 0, ignored_bad_result}, {"oversized", 0, ignored_oversized_result},
        {"throwing", 0, throwing_function}, {"failing", 0, failing_function},
        {"nothing", 0, undefined_function}, {"temporary", 0, local_result},
        {"unterminated", 0, unterminated_json}
    };
    auto source = definition();
    source.functions = functions;
    source.function_count = std::size(functions);
    runtime->install_module(FlatwormModule::from_static(source));
    ASSERT_TRUE(runtime->evaluate("globalThis.u = Flatworm.module('unit'); true").ok);
    EXPECT_EQ(runtime->evaluate("u.temporary()").value, "temporary buffer");
    EXPECT_EQ(runtime->evaluate("u.nothing()").value, "undefined");
    EXPECT_EQ(runtime->evaluate("u.unterminated().n").value, "1");
    for (const char* expression : {"u.bad()", "u.oversized()", "u.throwing()", "u.failing()"}) {
        const auto result = runtime->evaluate(expression);
        EXPECT_FALSE(result.ok);
        EXPECT_EQ(result.error.find("private-result-marker"), std::string::npos);
        EXPECT_EQ(result.error.find("private-exception-marker"), std::string::npos);
    }
    EXPECT_EQ(runtime->evaluate("u.temporary()").value, "temporary buffer");
}

struct Lifecycle {
    int initialized = 0;
    int shut_down = 0;
    bool fail = false;
    bool throw_initialize = false;
    bool throw_shutdown = false;
    std::vector<void*> shutdown_order;
} lifecycle;

FlatwormStatus initialize(const FlatwormModuleHostApi* api, void** instance) {
    EXPECT_EQ(api->abi_version, FLATWORM_MODULE_ABI_VERSION);
    EXPECT_GE(api->struct_size, sizeof(FlatwormModuleHostApi));
    *instance = &lifecycle;
    ++lifecycle.initialized;
    if (lifecycle.throw_initialize) throw std::runtime_error("private-initialize-marker");
    return lifecycle.fail ? FLATWORM_STATUS_ERROR : FLATWORM_STATUS_OK;
}
void shutdown(void* instance) {
    ++lifecycle.shut_down;
    lifecycle.shutdown_order.push_back(instance);
    if (lifecycle.throw_shutdown) throw std::runtime_error("private-shutdown-marker");
}

TEST_F(FlatwormModuleRuntime, InitializesOnceAndShutsDownAfterCapturedFunctionsAndJobs) {
    lifecycle = {};
    auto source = definition();
    source.initialize = initialize;
    source.shutdown = shutdown;
    auto module = FlatwormModule::from_static(source);
    runtime->install_module(module);
    module.reset();
    EXPECT_EQ(lifecycle.initialized, 1);
    EXPECT_EQ(lifecycle.shut_down, 0);
    ASSERT_TRUE(runtime->evaluate("globalThis.saved = Flatworm.module('unit').echo; saved(9)").ok);
    EXPECT_EQ(runtime->evaluate("saved(10)").value, "10");
    // Teardown with a queued native call verifies library/instance retention.
    prowsetk::ScriptOptions options;
    options.max_microtask_jobs = 1;
    EXPECT_FALSE(runtime->evaluate("queueMicrotask(() => saved(1)); queueMicrotask(() => saved(2));", options).ok);
    lifecycle.throw_shutdown = true;
    runtime.reset();
    EXPECT_EQ(lifecycle.shut_down, 1);
}

TEST_F(FlatwormModuleRuntime, FailedInitializeCleansPartialStateAndDoesNotPublishExports) {
    lifecycle = {};
    auto source = definition();
    source.initialize = initialize;
    source.shutdown = shutdown;
    auto module = FlatwormModule::from_static(source);
    for (bool throw_error : {false, true}) {
        lifecycle.fail = true;
        lifecycle.throw_initialize = throw_error;
        expect_error([&] { runtime->install_module(module); }, ErrorCode::JavaScriptError);
        EXPECT_EQ(lifecycle.initialized, lifecycle.shut_down);
        EXPECT_FALSE(runtime->evaluate("Flatworm.module('unit')").ok);
        EXPECT_EQ(runtime->modules().size(), 1u);
    }
    lifecycle.fail = lifecycle.throw_initialize = false;
    runtime->install_module(module);
    EXPECT_EQ(lifecycle.initialized, 3);
    runtime.reset();
    EXPECT_EQ(lifecycle.shut_down, 3);
    source.shutdown = nullptr;
    expect_error([&] { FlatwormModule::from_static(source); }, ErrorCode::InvalidArgument);
}

TEST_F(FlatwormModuleRuntime, InvalidJsonConstantFailsBeforeInitializationAndIsRetryable) {
    lifecycle = {};
    FlatwormConstant constant{"json", {FLATWORM_VALUE_JSON, 0, 0, {"private-json-marker", 19}}};
    auto source = definition();
    source.initialize = initialize;
    source.shutdown = shutdown;
    source.constants = &constant;
    source.constant_count = 1;
    expect_error([&] { runtime->install_module(FlatwormModule::from_static(source)); }, ErrorCode::JavaScriptError);
    EXPECT_EQ(lifecycle.initialized, 0);
    EXPECT_FALSE(runtime->evaluate("Flatworm.module('unit')").ok);
    constant.value.bytes = {"{}", 2};
    runtime->install_module(FlatwormModule::from_static(source));
    EXPECT_EQ(lifecycle.initialized, 1);
    runtime.reset();
    EXPECT_EQ(lifecycle.shut_down, 1);
}

int shutdown_first = 0;
int shutdown_second = 0;
FlatwormStatus initialize_first(const FlatwormModuleHostApi*, void** instance) {
    *instance = &shutdown_first;
    return FLATWORM_STATUS_OK;
}
FlatwormStatus initialize_second(const FlatwormModuleHostApi*, void** instance) {
    *instance = &shutdown_second;
    return FLATWORM_STATUS_OK;
}

TEST_F(FlatwormModuleRuntime, ShutsDownInReverseInstallationOrderAndBoundsRuntimeModules) {
    lifecycle = {};
    auto first = definition("first");
    first.initialize = initialize_first;
    first.shutdown = shutdown;
    auto second = definition("second");
    second.initialize = initialize_second;
    second.shutdown = shutdown;
    runtime->install_module(FlatwormModule::from_static(first));
    runtime->install_module(FlatwormModule::from_static(second));
    // The fixture plus the two stateful modules already occupy three slots.
    for (unsigned i = 3; i < FLATWORM_MODULE_MAX_MODULES; ++i) {
        const auto name = "m" + std::to_string(i);
        runtime->install_module(FlatwormModule::from_static(definition(name.c_str())));
    }
    expect_error([&] { runtime->install_module(FlatwormModule::from_static(definition("overflow"))); },
                 ErrorCode::ResourceLimit);
    runtime.reset();
    EXPECT_EQ(lifecycle.shutdown_order, (std::vector<void*>{&shutdown_second, &shutdown_first}));
}

TEST_F(FlatwormModuleRuntime, ModuleEvaluationRetainsDeadlinesAndBoundedMicrotasks) {
    prowsetk::ScriptOptions options;
    options.timeout_ms = 20;
    EXPECT_FALSE(runtime->evaluate_module("for (;;) {}", options).ok);
    EXPECT_EQ(runtime->evaluate("m.echo('after deadline')").value, "after deadline");
    options.timeout_ms = 5000;
    options.max_microtask_jobs = 1;
    const auto result = runtime->evaluate_module(
        "import {echo} from 'flatworm:fixture';"
        "queueMicrotask(() => echo(1)); queueMicrotask(() => echo(2));", options);
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("microtask job limit"), std::string::npos);
    EXPECT_TRUE(runtime->run_microtasks().ok);
}

prowsetk::JavaScriptRuntime* active_runtime = nullptr;
FlatwormStatus try_reentry(FlatwormCall* call) {
    bool rejected = !active_runtime->evaluate("throw 'private-reentry-marker'").ok;
    try {
        active_runtime->install_module(FlatwormModule::from_static(definition("reentrant")));
        rejected = false;
    } catch (const Error& error) {
        rejected = rejected && error.code() == ErrorCode::JavaScriptError;
    }
    const FlatwormValue value{FLATWORM_VALUE_BOOLEAN, rejected ? 1 : 0, 0, {}};
    return call->api->set_result(call, &value);
}

TEST_F(FlatwormModuleRuntime, RejectsReentrantEvaluationAndInstallation) {
    const FlatwormFunction function{"reentry", 0, try_reentry};
    auto source = definition();
    source.functions = &function;
    runtime->install_module(FlatwormModule::from_static(source));
    active_runtime = runtime.get();
    EXPECT_EQ(runtime->evaluate("Flatworm.module('unit').reentry()").value, "true");
    active_runtime = nullptr;
    EXPECT_FALSE(runtime->evaluate("Flatworm.module('reentrant')").ok);
    EXPECT_EQ(runtime->evaluate("m.echo(3)").value, "3");
}

}  // namespace
