#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <memory>
#include <stdexcept>
#include <string>

#include "prowsetk/browser.hpp"
#include "prowsetk/error.hpp"
#include "prowsetk/plugins/ai_oracle.hpp"
#include "prowsetk/plugins/ai_oracle.h"

namespace ai = prowsetk::plugins::ai_oracle;
using Json = nlohmann::json;
using prowsetk::Error;
using prowsetk::ErrorCode;

namespace {
ai::OracleOptions options() {
    ai::OracleOptions value;
    value.enabled = true;
    value.api_key = "fixture-key";
    return value;
}

ai::OracleRequest inquiry() {
    ai::OracleRequest value;
    value.prompt = "Which link should the spider visit next?";
    return value;
}

Json answer(std::string text = "Inspect the catalog link.") {
    return {{"id", "resp_fixture"}, {"model", "gpt-4o-mini"}, {"status", "completed"},
            {"output", Json::array({{{"type", "message"}, {"role", "assistant"}, {"status", "completed"},
                                    {"content", Json::array({{{"type", "output_text"}, {"text", text}}})}}})},
            {"usage", {{"input_tokens", 31}, {"output_tokens", 9}}}};
}

prowsetk::HttpResponse response(const Json& json = answer()) {
    prowsetk::HttpResponse value;
    value.status = 200;
    value.body = json.dump();
    return value;
}

template <typename F> void expect_error(ErrorCode code, F&& function) {
    try {
        function();
        FAIL() << "expected error";
    } catch (const Error& error) {
        EXPECT_EQ(error.code(), code) << error.what();
        EXPECT_EQ(std::string(error.what()).find("fixture-key"), std::string::npos);
        EXPECT_EQ(std::string(error.what()).find("private-fixture"), std::string::npos);
    }
}

class AiOracle : public testing::Test {
protected:
    prowsetk::MemoryNetworkClient network;
    void SetUp() override {
        network.set_response("https://api.openai.com/v1/responses", response());
    }
};
}  // namespace

TEST_F(AiOracle, SendsResponsesApiThroughHostWithLimitsAndOpenAIppHeaders) {
    auto config = options();
    config.organization = "org-fixture";
    config.project = "project-fixture";
    config.timeout_ms = 4321;
    config.max_response_bytes = 12000;
    config.max_output_tokens = 256;
    ai::Oracle oracle(network, config);
    auto request = inquiry();
    request.context_json = R"({"links":["/catalog"],"note":"quotes: \" and \\"})";
    const auto result = oracle.ask(request);
    EXPECT_EQ(result.answer, "Inspect the catalog link.");
    EXPECT_EQ(result.response_id, "resp_fixture");
    EXPECT_EQ(result.provenance, "openai-responses");
    EXPECT_TRUE(result.advisory);
    EXPECT_EQ(result.input_tokens, 31u);
    EXPECT_EQ(result.output_tokens, 9u);
    EXPECT_EQ(oracle.request_count(), 1u);
    ASSERT_EQ(network.requests().size(), 1u);
    const auto& http = network.requests().front();
    EXPECT_EQ(http.method, "POST");
    EXPECT_EQ(http.timeout_ms, 4321);
    EXPECT_EQ(http.max_response_bytes, 12000u);
    prowsetk::HttpResponse header_view;
    header_view.headers = http.headers;
    EXPECT_EQ(header_view.header("Authorization"), "Bearer fixture-key");
    EXPECT_EQ(header_view.header("OpenAI-Organization"), "org-fixture");
    EXPECT_EQ(header_view.header("OpenAI-Project"), "project-fixture");
    EXPECT_TRUE(header_view.header("Cookie").empty());
    const auto body = Json::parse(http.body);
    EXPECT_EQ(body["model"], "gpt-4o-mini");
    EXPECT_EQ(body["max_output_tokens"], 256);
    EXPECT_EQ(body["store"], false);
    EXPECT_EQ(body["stream"], false);
    const auto payload = Json::parse(body["input"][0]["content"][0]["text"].get<std::string>());
    EXPECT_EQ(payload["question"], request.prompt);
    EXPECT_EQ(payload["context"], Json::parse(request.context_json));
}

TEST_F(AiOracle, DisabledAndMissingKeyNeverContactTransport) {
    ai::Oracle disabled(network);
    expect_error(ErrorCode::SecurityViolation, [&] { disabled.ask(inquiry()); });
    auto config = options();
    config.api_key.clear();
    ai::Oracle missing(network, config);
    expect_error(ErrorCode::InvalidArgument, [&] { missing.ask(inquiry()); });
    EXPECT_TRUE(network.requests().empty());
}

TEST_F(AiOracle, ValidatesEndpointHeadersAndFiniteBudgets) {
    for (const auto* base : {"http://api.example/v1", "https://user:private-fixture@api.example/v1",
                             "https://api.example/v1?token=private-fixture", "https://api.example/#x",
                             "https://api.example:99999/v1", "https://api.example\\evil/v1", "file:///v1"}) {
        auto config = options();
        config.base_url = base;
        EXPECT_THROW(ai::Oracle(network, config), Error) << base;
    }
    auto config = options();
    config.api_key = "private-fixture\r\nCookie: x";
    expect_error(ErrorCode::InvalidArgument, [&] { ai::Oracle oracle(network, config); });
    config = options();
    config.timeout_ms = 0;
    expect_error(ErrorCode::InvalidArgument, [&] { ai::Oracle oracle(network, config); });
    config = options();
    config.max_requests = 0;
    expect_error(ErrorCode::InvalidArgument, [&] { ai::Oracle oracle(network, config); });
    config = options();
    config.max_input_bytes = 2u * 1024u * 1024u + 1;
    expect_error(ErrorCode::InvalidArgument, [&] { ai::Oracle oracle(network, config); });
    EXPECT_TRUE(network.requests().empty());
}

TEST_F(AiOracle, SanitizesDetachedPageAndStructuredContext) {
    auto document = prowsetk::parse_html(R"HTML(
      <!DOCTYPE private-fixture-declaration><!-- private-fixture-comment -->
      <script>private-fixture-script</script><style>private-fixture-style</style>
      <form action="/next?token=private-fixture-query&view=list">
        <input name="password" value="private-fixture-input">
        <textarea>private-fixture-textarea</textarea><select><option>private-fixture-option</option></select>
        <div contenteditable>private-fixture-editable</div>
        <a id="next" href="https://user:private-fixture-userinfo@example.test/catalog?api_key=private-fixture-key#private-fixture-fragment"
           onclick="private-fixture-handler" data-token="private-fixture-data">Catalog</a>
      </form>)HTML", "https://example.test/?access_token=private-fixture-page");
    const auto original = document->html();
    ai::Oracle oracle(network, options());
    auto request = inquiry();
    request.html = original;
    request.page_url = document->url();
    request.prompt += " fixture-key";
    request.context_json = R"({"headers":{"Authorization":"private-fixture-auth"},"password":"private-fixture-password","nested":[{"cookie":"private-fixture-cookie"}],"url":"https://example.test/?token=private-fixture-url","fixture-key":"key names are redacted too"})";
    oracle.ask(request);
    const auto& body = network.requests().front().body;
    EXPECT_EQ(body.find("private-fixture"), std::string::npos) << body;
    EXPECT_EQ(body.find("fixture-key"), std::string::npos);
    EXPECT_NE(body.find("Catalog"), std::string::npos);
    EXPECT_NE(body.find("view=list"), std::string::npos);
    EXPECT_EQ(document->html(), original);
    EXPECT_EQ(document->query_selector("input")->value(), "private-fixture-input");
}

TEST_F(AiOracle, DocumentHelperRetainsTaskAndUrlWithoutMutatingPage) {
    ai::Oracle oracle(network, options());
    const auto document = prowsetk::parse_html("<a href='/catalog'>Catalog</a>", "https://example.test/");
    oracle.ask_document(*document, "Inspect the next link", ai::Task::Crawl);
    const auto body = Json::parse(network.requests()[0].body);
    const auto payload = Json::parse(body["input"][0]["content"][0]["text"].get<std::string>());
    EXPECT_EQ(payload["task"], "crawl");
    EXPECT_EQ(payload["page_url"], "https://example.test/");
    EXPECT_EQ(document->links().size(), 1u);
}

TEST_F(AiOracle, RedactsMixedCaseAndHtmlEncodedUrlsWithoutCredentialBoundaryGaps) {
    network.set_response("https://api.openai.com/v1/responses", response(answer(
        "HTTPS://example.test/?api_key=fixture-key&token=private-fixture-answer")));
    ai::Oracle oracle(network, options());
    auto request = inquiry();
    request.prompt = "Inspect HtTpS://example.test/?api_key=fixture-key&token=private-fixture-prompt";
    request.page_url = "HTTPS://user:private-fixture-userinfo@example.test/?token=private-fixture-page";
    request.context_json = R"({"url":"https://[::1]/?api_key=fixture-key&token=private-fixture-context"})";
    request.html = "<p>https://example.test/?view=list&amp;token=private-fixture-text</p>";
    const auto result = oracle.ask(request);
    EXPECT_EQ(network.requests()[0].body.find("private-fixture"), std::string::npos);
    EXPECT_EQ(network.requests()[0].body.find("fixture-key"), std::string::npos);
    EXPECT_EQ(result.answer.find("private-fixture"), std::string::npos);
    EXPECT_EQ(result.answer.find("fixture-key"), std::string::npos);
    EXPECT_NE(network.requests()[0].body.find("view=list"), std::string::npos);

    auto config = options();
    config.api_key = "fixture&key";
    ai::Oracle encoded(network, config);
    network.set_response("https://api.openai.com/v1/responses", response(answer("fixture&key")));
    request = inquiry();
    request.html = "<p>fixture&amp;key</p>";
    EXPECT_EQ(encoded.ask(request).answer, "[REDACTED]");
    EXPECT_EQ(network.requests().back().body.find("fixture&amp;key"), std::string::npos);
}

TEST_F(AiOracle, CaptchaAcceptsOnlyBoundedEmbeddedImages) {
    ai::Oracle oracle(network, options());
    auto request = inquiry();
    request.task = ai::Task::Captcha;
    request.images = {"data:image/png;base64,aW1hZ2U="};
    oracle.ask(request);
    const auto body = Json::parse(network.requests()[0].body);
    EXPECT_EQ(body["input"][0]["content"][1]["type"], "input_image");
    EXPECT_EQ(body["input"][0]["content"][1]["image_url"], request.images[0]);
    for (const auto* image : {"https://example.test/image.png", "data:image/svg+xml;base64,aW1hZ2U=",
                              "data:image/png;base64,abc", "data:image/png;base64,ab=c", "data:image/png;base64,!!!!"}) {
        request.images = {image};
        expect_error(ErrorCode::InvalidArgument, [&] { oracle.ask(request); });
    }
    request.images.assign(5, "data:image/png;base64,aW1hZ2U=");
    expect_error(ErrorCode::InvalidArgument, [&] { oracle.ask(request); });
    EXPECT_EQ(network.requests().size(), 1u);
}

TEST_F(AiOracle, CountsFailedAttemptsAndDoesNotRetryRateLimits) {
    network.set_handler([](const auto&) {
        auto value = response();
        value.status = 429;
        value.body = "private-fixture and fixture-key";
        return value;
    });
    auto config = options();
    config.max_requests = 1;
    ai::Oracle oracle(network, config);
    expect_error(ErrorCode::NetworkError, [&] { oracle.ask(inquiry()); });
    expect_error(ErrorCode::ResourceLimit, [&] { oracle.ask(inquiry()); });
    EXPECT_EQ(network.requests().size(), 1u);
    EXPECT_EQ(oracle.request_count(), 1u);
}

TEST_F(AiOracle, TransportExceptionsKeepCodesAndSuppressPrivateMessages) {
    network.set_handler([](const auto&) -> prowsetk::HttpResponse {
        throw Error(ErrorCode::Timeout, "private-fixture fixture-key");
    });
    ai::Oracle oracle(network, options());
    expect_error(ErrorCode::Timeout, [&] { oracle.ask(inquiry()); });
    network.set_handler([](const auto&) -> prowsetk::HttpResponse {
        throw std::runtime_error("private-fixture fixture-key");
    });
    expect_error(ErrorCode::NetworkError, [&] { oracle.ask(inquiry()); });
    EXPECT_EQ(oracle.request_count(), 2u);
}

TEST_F(AiOracle, RejectsRedirectsWithoutFollowingThem) {
    for (int mode = 0; mode < 3; ++mode) {
        network.set_handler([mode](const auto&) {
            auto value = response();
            if (mode == 0) value.status = 307;
            if (mode == 1) value.final_url = "https://other.test/private-fixture";
            if (mode == 2) value.redirect_chain = {"https://api.openai.com/v1/responses"};
            return value;
        });
        ai::Oracle oracle(network, options());
        expect_error(ErrorCode::SecurityViolation, [&] { oracle.ask(inquiry()); });
    }
    EXPECT_EQ(network.requests().size(), 3u);
    for (const auto& http : network.requests()) EXPECT_EQ(http.url, "https://api.openai.com/v1/responses");
}

TEST_F(AiOracle, RejectsMalformedIncompleteRefusedAndToolOutputs) {
    for (const auto& json : {
        Json{{"status", "incomplete"}, {"output", Json::array()}},
        Json{{"status", "completed"}, {"output", Json::array({{{"type", "function_call"}}})}},
        Json{{"status", "completed"}, {"output", Json::array({{{"type", "message"}, {"role", "assistant"},
            {"status", "completed"}, {"content", Json::array({{{"type", "refusal"}, {"refusal", "private-fixture"}}})}}})}},
        Json{{"status", "completed"}, {"output", Json::array()}}}) {
        network.set_response("https://api.openai.com/v1/responses", response(json));
        ai::Oracle oracle(network, options());
        expect_error(ErrorCode::ParseError, [&] { oracle.ask(inquiry()); });
    }
    network.set_handler([](const auto&) { auto value = response(); value.body = "private-fixture invalid JSON"; return value; });
    ai::Oracle oracle(network, options());
    expect_error(ErrorCode::ParseError, [&] { oracle.ask(inquiry()); });
}

TEST_F(AiOracle, HandlesReasoningAndMultipartAnswerAndRedactsStructuredOutput) {
    auto json = answer(R"({"next":"/catalog",)");
    json["output"].insert(json["output"].begin(), Json{{"type", "reasoning"}, {"summary", Json::array()}});
    json["output"][1]["content"].push_back({{"type", "output_text"}, {"text", R"("token":"private-fixture","note":"fixture-key"})"}});
    network.set_response("https://api.openai.com/v1/responses", response(json));
    ai::Oracle oracle(network, options());
    auto request = inquiry();
    request.json_output = true;
    const auto result = oracle.ask(request);
    EXPECT_EQ(Json::parse(result.answer)["next"], "/catalog");
    EXPECT_EQ(Json::parse(result.answer)["token"], "[REDACTED]");
    EXPECT_EQ(Json::parse(result.answer)["note"], "[REDACTED]");
    const auto body = Json::parse(network.requests()[0].body);
    EXPECT_EQ(body["text"]["format"]["type"], "json_object");
}

TEST_F(AiOracle, RequiresObjectForJsonOutputAndRejectsInvalidUsage) {
    network.set_response("https://api.openai.com/v1/responses", response(answer("[1,2]")));
    ai::Oracle oracle(network, options());
    auto request = inquiry();
    request.json_output = true;
    expect_error(ErrorCode::ParseError, [&] { oracle.ask(request); });
    auto json = answer();
    json["usage"]["input_tokens"] = -1;
    network.set_response("https://api.openai.com/v1/responses", response(json));
    expect_error(ErrorCode::ParseError, [&] { oracle.ask(inquiry()); });
}

TEST_F(AiOracle, BoundsRawInputResponseAndJsonNesting) {
    auto config = options();
    config.max_input_bytes = 64;
    ai::Oracle oracle(network, config);
    auto request = inquiry();
    request.html = std::string(64, 'x');
    expect_error(ErrorCode::ResourceLimit, [&] { oracle.ask(request); });
    EXPECT_EQ(oracle.request_count(), 0u);
    config = options();
    config.max_response_bytes = 10;
    ai::Oracle small_response(network, config);
    expect_error(ErrorCode::ResourceLimit, [&] { small_response.ask(inquiry()); });
    ai::Oracle nested(network, options());
    request = inquiry();
    request.context_json = "{\"nested\":" + std::string(40, '[') + "0" + std::string(40, ']') + "}";
    expect_error(ErrorCode::ResourceLimit, [&] { nested.ask(request); });
    request.context_json = "[]";
    expect_error(ErrorCode::InvalidArgument, [&] { nested.ask(request); });
    request.context_json = "{private-fixture";
    expect_error(ErrorCode::ParseError, [&] { nested.ask(request); });
}

TEST_F(AiOracle, RegistryLoadingIsNetworkFreeAndKeepsVersionTwoAbi) {
    prowsetk::Browser browser;
    const auto& descriptor = browser.plugins().load_native(AI_ORACLE_PLUGIN_PATH);
    EXPECT_EQ(descriptor.name, "ai-oracle");
    EXPECT_EQ(descriptor.abi_version, "2");
    EXPECT_EQ(descriptor.lua_module, "ai_oracle");
    EXPECT_EQ(browser.plugins().initialize_all(), 1u);
    EXPECT_TRUE(browser.plugins().has_capability("ai-oracle"));
    EXPECT_TRUE(browser.plugins().has_capability("captcha-advice"));
    browser.create_session()->load_html("<p>Offline</p>");
    browser.plugins().shutdown_all();
}

extern "C" int test_ai_oracle_c_contract(void);

TEST(AiOracleC, HostCallbackAndOpaqueOwnershipWorkFromC) {
    EXPECT_EQ(test_ai_oracle_c_contract(), 0);
}
