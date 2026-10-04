#include <gtest/gtest.h>

#include <algorithm>
#include <initializer_list>
#include <limits>
#include <memory>
#include <string>
#include <string_view>

#include "Flatwork-Module.h"
#include "module.hpp"
#include "prowsetk/flatworm_module.hpp"
#include "prowsetk/javascript_runtime.hpp"

namespace {

FlatwormValue text(std::string_view value) {
    return {FLATWORM_VALUE_STRING, 0, 0, {value.data(), value.size()}};
}
FlatwormValue json(std::string_view value) {
    return {FLATWORM_VALUE_JSON, 0, 0, {value.data(), value.size()}};
}
FlatwormValue number(double value) { return {FLATWORM_VALUE_NUMBER, 0, value, {nullptr, 0}}; }
FlatwormValue null_value() { return {FLATWORM_VALUE_NULL, 0, 0, {nullptr, 0}}; }
FlatwormValue undefined() { return {FLATWORM_VALUE_UNDEFINED, 0, 0, {nullptr, 0}}; }

struct Capture {
    FlatwormStatus status = FLATWORM_STATUS_OK;
    std::string bytes;
    static FlatwormStatus set_result(FlatwormCall* call, const FlatwormValue* value) {
        auto& capture = *static_cast<Capture*>(call->host_data);
        if (capture.status != FLATWORM_STATUS_OK) return capture.status;
        if (value == nullptr || value->type != FLATWORM_VALUE_JSON || value->bytes.data == nullptr)
            return FLATWORM_STATUS_INVALID_ARGUMENT;
        capture.bytes.assign(value->bytes.data, value->bytes.size);
        return FLATWORM_STATUS_OK;
    }
};

const FlatwormModuleHostApi host{
    FLATWORM_MODULE_ABI_VERSION, sizeof(FlatwormModuleHostApi), Capture::set_result};

class RpcModule : public ::testing::Test {
protected:
    void SetUp() override { ASSERT_EQ(definition.initialize(&host, &instance), FLATWORM_STATUS_OK); }
    void TearDown() override { definition.shutdown(instance); }
    FlatwormStatus call(std::string_view name, std::initializer_list<FlatwormValue> args) {
        capture.bytes.clear();
        const auto end = definition.functions + definition.function_count;
        const auto* function = std::find_if(definition.functions, end,
            [&](const FlatwormFunction& item) { return name == item.name; });
        if (function == end) return FLATWORM_STATUS_ERROR;
        FlatwormCall invocation{&host, instance, args.begin(), args.size(), &capture};
        return function->invoke(&invocation);
    }
    void expect(std::string_view name, std::initializer_list<FlatwormValue> args,
                std::string_view output) {
        ASSERT_EQ(call(name, args), FLATWORM_STATUS_OK);
        EXPECT_EQ(capture.bytes, output);
    }
    const FlatwormModuleDefinition& definition = flatworm::rpc::module_definition();
    void* instance = nullptr;
    Capture capture;
};

TEST(RpcModuleDefinition, ExposesTheIndependentModuleAbiAndValidatesHostServices) {
    const auto& definition = flatworm::rpc::module_definition();
    EXPECT_EQ(definition.abi_version, FLATWORM_MODULE_ABI_VERSION);
    EXPECT_EQ(definition.struct_size, sizeof(FlatwormModuleDefinition));
    EXPECT_STREQ(definition.name, "rpc");
    EXPECT_EQ(definition.function_count, 8u);
    EXPECT_EQ(definition.constant_count, 10u);
    void* instance = nullptr;
    EXPECT_EQ(definition.initialize(nullptr, &instance), FLATWORM_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(instance, nullptr);
    EXPECT_EQ(definition.initialize(&host, nullptr), FLATWORM_STATUS_INVALID_ARGUMENT);
    auto invalid_host = host;
    invalid_host.abi_version++;
    EXPECT_EQ(definition.initialize(&invalid_host, &instance), FLATWORM_STATUS_INVALID_ARGUMENT);
    invalid_host = host;
    invalid_host.struct_size = 0;
    EXPECT_EQ(definition.initialize(&invalid_host, &instance), FLATWORM_STATUS_INVALID_ARGUMENT);
    invalid_host = host;
    invalid_host.set_result = nullptr;
    EXPECT_EQ(definition.initialize(&invalid_host, &instance), FLATWORM_STATUS_INVALID_ARGUMENT);
    definition.shutdown(nullptr);
}

TEST_F(RpcModule, RejectsMalformedAbiCallsBeforeReadingArgumentsOrWritingResults) {
    const auto function = definition.functions[0].invoke;
    EXPECT_EQ(function(nullptr), FLATWORM_STATUS_INVALID_ARGUMENT);
    FlatwormCall invocation{&host, instance, nullptr, 1, &capture};
    EXPECT_EQ(function(&invocation), FLATWORM_STATUS_INVALID_ARGUMENT);
    const auto method = text("next");
    invocation.arguments = &method;
    invocation.instance = nullptr;
    EXPECT_EQ(function(&invocation), FLATWORM_STATUS_INVALID_ARGUMENT);
    invocation.instance = instance;
    invocation.api = nullptr;
    EXPECT_EQ(function(&invocation), FLATWORM_STATUS_INVALID_ARGUMENT);
    invocation.api = &host;
    invocation.argument_count = std::numeric_limits<std::size_t>::max();
    EXPECT_EQ(function(&invocation), FLATWORM_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(call("request", {{99, 0, 0, {nullptr, 0}}}), FLATWORM_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(call("request", {{FLATWORM_VALUE_STRING, 0, 0, {nullptr, 1}}}), FLATWORM_STATUS_INVALID_ARGUMENT);
    EXPECT_TRUE(capture.bytes.empty());
}

TEST_F(RpcModule, BuildsPositionalAndNamedRequestsWithTransactionalInstanceIds) {
    expect("request", {text("subtract"), json("[42,23]")},
        R"({"jsonrpc":"2.0","method":"subtract","params":[42,23],"id":1})");
    expect("request", {text("subtract"), json(R"({"minuend":42,"subtrahend":23})")},
        R"({"jsonrpc":"2.0","method":"subtract","params":{"minuend":42,"subtrahend":23},"id":2})");
    expect("request", {text("get_data"), undefined(), text("custom")},
        R"({"jsonrpc":"2.0","method":"get_data","id":"custom"})");
    expect("request", {text("get_data"), undefined(), null_value()},
        R"({"jsonrpc":"2.0","method":"get_data","id":null})");
    EXPECT_EQ(call("request", {text("bad"), null_value()}), FLATWORM_STATUS_INVALID_ARGUMENT);
    capture.status = FLATWORM_STATUS_RESOURCE_LIMIT;
    EXPECT_EQ(call("request", {text("retry")}), FLATWORM_STATUS_RESOURCE_LIMIT);
    capture.status = FLATWORM_STATUS_OK;
    expect("request", {text("retry")}, R"({"jsonrpc":"2.0","method":"retry","id":3})");
}

TEST_F(RpcModule, NotificationsOmitIdsAndDoNotAdvanceTheRequestCounter) {
    expect("notification", {text("update"), json("[1,2,3]")},
        R"({"jsonrpc":"2.0","method":"update","params":[1,2,3]})");
    expect("notification", {text("tick")}, R"({"jsonrpc":"2.0","method":"tick"})");
    expect("request", {text("next")}, R"({"jsonrpc":"2.0","method":"next","id":1})");
    EXPECT_EQ(call("notification", {text("tick"), undefined(), number(1)}), FLATWORM_STATUS_INVALID_ARGUMENT);
}

TEST_F(RpcModule, RejectsInvalidMethodParamsIdsAndArgumentCounts) {
    EXPECT_EQ(call("request", {}), FLATWORM_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(call("request", {number(1)}), FLATWORM_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(call("request", {text("rpc.internal")}), FLATWORM_STATUS_INVALID_ARGUMENT);
    for (auto params : {text("params"), number(1), null_value(), json("true")})
        EXPECT_EQ(call("request", {text("method"), params}), FLATWORM_STATUS_INVALID_ARGUMENT);
    for (auto id : {json("true"), json("[]"), json("{}"), number(1.5),
                   number(9007199254740992.0), number(std::numeric_limits<double>::infinity())})
        EXPECT_EQ(call("request", {text("method"), undefined(), id}), FLATWORM_STATUS_INVALID_ARGUMENT);
    const std::string long_method(1025, 'x');
    EXPECT_EQ(call("request", {text(long_method)}), FLATWORM_STATUS_RESOURCE_LIMIT);
    expect("request", {text("RPC.extension")}, R"({"jsonrpc":"2.0","method":"RPC.extension","id":1})");
}

TEST_F(RpcModule, BuildsSuccessAndStandardErrorResponsesIncludingNullResults) {
    expect("result", {number(1), number(19)}, R"({"jsonrpc":"2.0","id":1,"result":19})");
    expect("result", {text("id"), null_value()}, R"({"jsonrpc":"2.0","id":"id","result":null})");
    expect("result", {null_value(), json("false")}, R"({"jsonrpc":"2.0","id":null,"result":false})");
    expect("error", {null_value(), number(-32700), text("Parse error")},
        R"({"jsonrpc":"2.0","id":null,"error":{"code":-32700,"message":"Parse error"}})");
    expect("error", {text("one"), number(-32602), text("Invalid params"), json(R"({"field":"count"})")},
        R"({"jsonrpc":"2.0","id":"one","error":{"code":-32602,"message":"Invalid params","data":{"field":"count"}}})");
    for (auto code : {number(1.5), json("true"), number(9007199254740992.0)})
        EXPECT_EQ(call("error", {number(1), code, text("error")}), FLATWORM_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(call("error", {number(1), number(-32603), number(5)}), FLATWORM_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(call("result", {number(1), undefined()}), FLATWORM_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(call("result", {number(1), number(std::numeric_limits<double>::quiet_NaN())}),
        FLATWORM_STATUS_INVALID_ARGUMENT);
}

TEST_F(RpcModule, ValidatesWireRequestsAndBatchesWithoutTreatingNullIdsAsNotifications) {
    const auto request = R"({"jsonrpc":"2.0","method":"subtract","params":[42,23],"id":null})";
    expect("parseRequest", {text(request)}, request);
    const auto batch = R"([{"jsonrpc":"2.0","method":"tick"},{"jsonrpc":"2.0","method":"get","id":1}])";
    expect("batch", {json(batch)}, batch);
    expect("parseRequest", {text(batch)}, batch);
    for (const char* source : {"[]", "[1]", "null", "{}",
            R"({"jsonrpc":"1.0","method":"foo"})", R"({"jsonrpc":"2.0","method":1})",
            R"({"jsonrpc":"2.0","method":"foo","params":null})",
            R"({"jsonrpc":"2.0","method":"foo","id":true})",
            R"({"jsonrpc":"2.0","method":"foo","result":1})"})
        EXPECT_EQ(call("parseRequest", {text(source)}), FLATWORM_STATUS_INVALID_ARGUMENT) << source;
    const auto duplicate = R"([{"jsonrpc":"2.0","method":"a","id":1},{"jsonrpc":"2.0","method":"b","id":1e0}])";
    EXPECT_EQ(call("batch", {json(duplicate)}), FLATWORM_STATUS_INVALID_ARGUMENT);
    // Parsing is structural; duplicate-id correlation policy belongs to batch/correlate.
    expect("parseRequest", {text(duplicate)}, duplicate);
}

TEST_F(RpcModule, ValidatesResponseEnvelopesAndTypeSensitiveExpectedIds) {
    expect("parseResponse", {text(R"({"jsonrpc":"2.0","id":1e0,"result":null})"), number(1)},
        R"({"jsonrpc":"2.0","id":1e0,"result":null})");
    expect("parseResponse", {json(R"({"jsonrpc":"2.0","id":"1","error":{"code":-32601,"message":"missing"}})"), text("1")},
        R"({"jsonrpc":"2.0","id":"1","error":{"code":-32601,"message":"missing"}})");
    for (const char* source : {"[]", "null", "{}", R"({"jsonrpc":"2.0","result":1})",
            R"({"jsonrpc":"2.0","id":1})", R"({"jsonrpc":"2.0","id":1,"result":1,"error":{}})",
            R"({"jsonrpc":"1.0","id":1,"result":1})", R"({"jsonrpc":"2.0","id":true,"result":1})",
            R"({"jsonrpc":"2.0","id":1,"error":null})",
            R"({"jsonrpc":"2.0","id":1,"error":{"code":1.5,"message":"x"}})",
            R"({"jsonrpc":"2.0","id":1,"error":{"code":1,"message":2}})",
            R"({"jsonrpc":"2.0","id":1,"result":1,"method":"x"})"})
        EXPECT_EQ(call("parseResponse", {text(source)}), FLATWORM_STATUS_INVALID_ARGUMENT) << source;
    EXPECT_EQ(call("parseResponse", {text(R"({"jsonrpc":"2.0","id":1,"result":1})"), text("1")}),
        FLATWORM_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(call("parseResponse", {text(R"({"jsonrpc":"2.0","id":1,"result":1})"), number(2)}),
        FLATWORM_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(call("parseResponse", {text(R"([{ "jsonrpc":"2.0","id":1,"result":1}])"), number(1)}),
        FLATWORM_STATUS_INVALID_ARGUMENT);
}

TEST_F(RpcModule, NumericIdsAndErrorCodesMustBeExactlyIntegralBeforeBinary64Rounding) {
    for (const char* id : {"1.00000000000000001", "9007199254740991.1", "-9007199254740991.1",
                          "0.99999999999999999", "99.999999999999999e-2"}) {
        const auto source = std::string(R"({"jsonrpc":"2.0","id":)") + id + R"(,"result":null})";
        EXPECT_EQ(call("parseResponse", {text(source)}), FLATWORM_STATUS_INVALID_ARGUMENT) << id;
        EXPECT_EQ(call("error", {number(1), json(id), text("error")}), FLATWORM_STATUS_INVALID_ARGUMENT) << id;
    }
    for (const char* id : {"1.0000", "1000e-3", "0.001e3", "-0e100", "9007199254740991.0",
                          "-90071992547409910e-1"}) {
        const auto source = std::string(R"({"jsonrpc":"2.0","id":)") + id + R"(,"result":null})";
        expect("parseResponse", {text(source)}, source);
    }
    expect("correlate", {json(R"({"jsonrpc":"2.0","method":"zero","id":-0})"),
                         json(R"({"jsonrpc":"2.0","id":0e0,"result":true})")},
        R"([{"jsonrpc":"2.0","id":0e0,"result":true}])");
}

TEST_F(RpcModule, CorrelatesOutOfOrderBatchesAndRejectsMissingUnexpectedOrDuplicateReplies) {
    const auto requests = R"([{"jsonrpc":"2.0","method":"a","id":1},{"jsonrpc":"2.0","method":"tick"},{"jsonrpc":"2.0","method":"b","id":"1"}])";
    const auto responses = R"([{"jsonrpc":"2.0","id":"1","error":{"code":-32601,"message":"missing"}},{"jsonrpc":"2.0","id":1,"result":"a"}])";
    expect("correlate", {json(requests), text(responses)},
        R"([{"jsonrpc":"2.0","id":1,"result":"a"},{"jsonrpc":"2.0","id":"1","error":{"code":-32601,"message":"missing"}}])");
    for (const char* replies : {"[]", R"({"jsonrpc":"2.0","id":1,"result":"a"})",
            R"([{"jsonrpc":"2.0","id":1,"result":"a"}])",
            R"([{"jsonrpc":"2.0","id":1,"result":"a"},{"jsonrpc":"2.0","id":1,"result":"duplicate"}])",
            R"([{"jsonrpc":"2.0","id":1,"result":"a"},{"jsonrpc":"2.0","id":2,"result":"unknown"}])"})
        EXPECT_EQ(call("correlate", {json(requests), text(replies)}), FLATWORM_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(call("correlate", {json(requests)}), FLATWORM_STATUS_INVALID_ARGUMENT);
    const auto null_id = R"({"jsonrpc":"2.0","method":"a","id":null})";
    expect("correlate", {json(null_id), json(R"({"jsonrpc":"2.0","id":null,"result":false})")},
        R"([{"jsonrpc":"2.0","id":null,"result":false}])");
}

TEST_F(RpcModule, NotificationOnlyBatchesExpectNoResponseIncludingNoEmptyResponseArray) {
    const auto requests = R"([{"jsonrpc":"2.0","method":"tick"}])";
    for (auto absent : {undefined(), null_value(), text(""), text(" \t\r\n")})
        expect("correlate", {json(requests), absent}, "[]");
    expect("correlate", {json(requests)}, "[]");
    for (auto replies : {json("[]"), json("null"), json(R"({"jsonrpc":"2.0","id":null,"result":1})")})
        EXPECT_EQ(call("correlate", {json(requests), replies}), FLATWORM_STATUS_INVALID_ARGUMENT);
}

TEST_F(RpcModule, StrictJsonRejectsDuplicateKeysMalformedNumbersAndTrailingInput) {
    for (const char* source : {
            R"({"jsonrpc":"2.0","id":1,"id":2,"result":null})",
            R"({"jsonrpc":"2.0","id":1,"\u0069d":2,"result":null})",
            R"({"jsonrpc":"2.0","id":01,"result":null})",
            R"({"jsonrpc":"2.0","id":+1,"result":null})",
            R"({"jsonrpc":"2.0","id":1.,"result":null})",
            R"({"jsonrpc":"2.0","id":1e,"result":null})",
            R"({"jsonrpc":"2.0","id":NaN,"result":null})",
            R"({"jsonrpc":"2.0","id":1,"result":1e9999})",
            R"({"jsonrpc":"2.0","id":1,"result":[1,]})",
            R"({"jsonrpc":"2.0","id":1,"result":null,})",
            R"({"jsonrpc":"2.0","id":1,"result":null} private-marker)",
            R"({"jsonrpc":"2.0","id":1,"result":"\x41"})",
            R"({"jsonrpc":"2.0","id":1,"result":"\ud800"})",
            R"({"jsonrpc":"2.0","id":1,"result":"\udc00"})"})
        EXPECT_EQ(call("parseResponse", {text(source)}), FLATWORM_STATUS_INVALID_ARGUMENT) << source;
    const std::string nul_suffix = std::string(R"({"jsonrpc":"2.0","id":1,"result":null})") + '\0';
    EXPECT_EQ(call("parseResponse", {text(nul_suffix)}), FLATWORM_STATUS_INVALID_ARGUMENT);
}

TEST_F(RpcModule, UnicodeAndEmbeddedNulsRoundTripWithoutTruncation) {
    const std::string id("x\0é", 4);
    expect("request", {text("méthod"), json(R"({"text":"\ud83d\ude00","\u0061":"x\u0000y"})"), text(id)},
        R"({"jsonrpc":"2.0","method":"méthod","params":{"text":"😀","a":"x\u0000y"},"id":"x\u0000é"})");
    expect("parseResponse", {text(R"({"jsonrpc":"2.0","id":"x\u0000é","result":"\ud83d\ude00"})"), text(id)},
        R"({"jsonrpc":"2.0","id":"x\u0000é","result":"😀"})");
    for (const auto& invalid_utf8 : {std::string("\xc0\xaf", 2), std::string("\xed\xa0\x80", 3),
                                  std::string("\xf4\x90\x80\x80", 4), std::string("\xc2", 1)}) {
        EXPECT_EQ(call("request", {text(invalid_utf8)}), FLATWORM_STATUS_INVALID_ARGUMENT);
        const auto source = std::string(R"({"jsonrpc":"2.0","id":1,"result":")") + invalid_utf8 + "\"}";
        EXPECT_EQ(call("parseResponse", {text(source)}), FLATWORM_STATUS_INVALID_ARGUMENT);
    }
}

TEST_F(RpcModule, BoundsBatchDepthValueCountsAndEncodedOutputAndRecovers) {
    std::string requests = "[";
    for (int i = 0; i < 128; ++i) {
        if (i != 0) requests += ',';
        requests += R"({"jsonrpc":"2.0","method":"tick"})";
    }
    requests += ']';
    EXPECT_EQ(call("batch", {json(requests)}), FLATWORM_STATUS_OK);
    requests.insert(requests.size() - 1, R"(,{"jsonrpc":"2.0","method":"tick"})");
    EXPECT_EQ(call("batch", {json(requests)}), FLATWORM_STATUS_RESOURCE_LIMIT);
    const auto within_depth = std::string(63, '[') + "null" + std::string(63, ']');
    EXPECT_EQ(call("result", {number(1), json(within_depth)}), FLATWORM_STATUS_OK);
    const auto over_depth = '[' + within_depth + ']';
    EXPECT_EQ(call("result", {number(1), json(over_depth)}), FLATWORM_STATUS_RESOURCE_LIMIT);
    std::string many_values = "[";
    for (int i = 0; i < 16383; ++i) { if (i != 0) many_values += ','; many_values += "null"; }
    many_values += ']';
    EXPECT_EQ(call("result", {number(1), json(many_values)}), FLATWORM_STATUS_RESOURCE_LIMIT);
    const std::string expansion(200000, '\0');
    EXPECT_EQ(call("result", {number(1), text(expansion)}), FLATWORM_STATUS_RESOURCE_LIMIT);
    const std::string oversized(FLATWORM_MODULE_MAX_VALUE_BYTES + 1, 'x');
    EXPECT_EQ(call("parseResponse", {text(oversized)}),
        FLATWORM_STATUS_RESOURCE_LIMIT);
    const std::string large_part(FLATWORM_MODULE_MAX_VALUE_BYTES / 2 + 1, 'x');
    EXPECT_EQ(call("error", {number(1), number(1), text(large_part), text(large_part)}),
        FLATWORM_STATUS_RESOURCE_LIMIT);
    expect("request", {text("after")}, R"({"jsonrpc":"2.0","method":"after","id":1})");
}

class RpcJavaScript : public ::testing::Test {
protected:
    void SetUp() override {
        runtime = prowsetk::make_javascript_runtime();
        if (runtime->name() == "null") GTEST_SKIP();
        runtime->install_module(prowsetk::FlatwormModule::from_static(flatworm::rpc::module_definition()));
        ASSERT_TRUE(runtime->evaluate("globalThis.rpc = Flatworm.module('rpc'); true").ok);
    }
    void check(std::string_view script) {
        const auto result = runtime->evaluate(script);
        ASSERT_TRUE(result.ok) << result.error;
        EXPECT_EQ(result.value, "true");
    }
    std::unique_ptr<prowsetk::JavaScriptRuntime> runtime;
};

TEST_F(RpcJavaScript, TransfersNativeObjectsWithoutMutatingInputAndFreezesExports) {
    check(R"js(
        (() => {
            const params = {a: [20, 22], label: 'é\u0000😀'};
            const message = rpc.request('add', params, 'custom');
            message.params.a.push(99);
            return params.a.length === 2 && message.params.label === params.label &&
                Object.isFrozen(rpc) && Object.getPrototypeOf(rpc) === null &&
                rpc.JSONRPC_VERSION === '2.0' && rpc.PARSE_ERROR === -32700 &&
                rpc.INVALID_REQUEST === -32600 && rpc.METHOD_NOT_FOUND === -32601 &&
                rpc.INVALID_PARAMS === -32602 && rpc.INTERNAL_ERROR === -32603 &&
                rpc.SERVER_ERROR_MIN === -32099 && rpc.SERVER_ERROR_MAX === -32000 &&
                rpc.MAX_BATCH_SIZE === 128 && rpc.MAX_JSON_DEPTH === 64 &&
                rpc.request.length === 3 && rpc.error.length === 4 &&
                !Object.hasOwn(rpc.notification('tick'), 'id');
        })()
    )js");
    check(R"js(
        (() => {
            const response = rpc.parseResponse('{"jsonrpc":"2.0","id":1,"result":{"__proto__":{"polluted":true}}}');
            return Object.hasOwn(response.result, '__proto__') && ({}).polluted === undefined;
        })()
    )js");
    EXPECT_FALSE(runtime->evaluate("'use strict'; rpc.request = 1").ok);
    EXPECT_FALSE(runtime->evaluate("'use strict'; rpc.MAX_BATCH_SIZE = 999").ok);
    check("rpc.request('ok').id === 1");
}

TEST_F(RpcJavaScript, ValidatesErrorRepliesAndCorrelatesAllCallsButNoNotifications) {
    check(R"js(
        (() => {
            const messages = rpc.batch([rpc.request('sum', [1, 2]), rpc.notification('tick'), rpc.request('unknown')]);
            const failed = rpc.error(messages[2].id, rpc.METHOD_NOT_FOUND, 'Method not found', {reason: 'unknown'});
            const replies = rpc.correlate(messages, [failed, rpc.result(messages[0].id, 3)]);
            return replies.length === 2 && replies[0].result === 3 && replies[1].error.code === -32601 &&
                replies[1].error.data.reason === 'unknown' &&
                rpc.parseResponse(failed, messages[2].id).id === messages[2].id &&
                rpc.correlate([rpc.notification('tick')], '').length === 0 &&
                rpc.parseRequest(messages).length === 3;
        })()
    )js");
}

TEST_F(RpcJavaScript, RejectsInvalidInputWithValueFreeErrorsAndRecoversAfterBoundsFailures) {
    for (const char* script : {
            "rpc.request('private-rpc-marker', 'private-rpc-marker')",
            "rpc.parseResponse('private-rpc-marker')",
            "rpc.parseResponse({jsonrpc:'2.0',id:1,result:1}, 'private-rpc-marker')",
            "rpc.request('private-rpc-marker', {toJSON(){throw new Error('private-rpc-marker')}})",
            "rpc.result(1, undefined)", "rpc.request('x', undefined, 1n)",
            "rpc.request('x', undefined, NaN)", "rpc.request('x', undefined, 1.5)",
            "rpc.batch([])", "rpc.result(1, '\\u0000'.repeat(200000))",
            "rpc.batch(Array(129).fill({jsonrpc:'2.0',method:'tick'}))"}) {
        const auto result = runtime->evaluate(script);
        EXPECT_FALSE(result.ok) << script;
        EXPECT_EQ(result.error.find("private-rpc-marker"), std::string::npos) << result.error;
    }
    check("rpc.request('after').id === 1");
}

}  // namespace
