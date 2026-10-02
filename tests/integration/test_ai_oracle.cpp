#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <memory>
#include <string>

#include "prowsetk/browser.hpp"
#include "prowsetk/lua_runtime.hpp"
#include "prowsetk/plugins/ai_oracle.h"
#include "prowsetk/plugins/ai_oracle.hpp"

#if defined(__unix__) || defined(__APPLE__)
#include <dlfcn.h>
#endif

namespace ai = prowsetk::plugins::ai_oracle;
using Json = nlohmann::json;

namespace {
std::string answer(std::string text) {
    return Json{{"id", "resp_fixture"}, {"model", "fixture-model"}, {"status", "completed"},
                {"output", Json::array({{{"type", "message"}, {"role", "assistant"}, {"status", "completed"},
                    {"content", Json::array({{{"type", "output_text"}, {"text", text}}})}}})}}.dump();
}

class AiOracleLua : public testing::Test {
protected:
    // Bound browsers outlive the Lua handles that reference their sessions.
    std::unique_ptr<prowsetk::Browser> bound_browser;
    prowsetk::LuaRuntime lua;
    void SetUp() override {
#ifdef AI_ORACLE_LUA_PATH
        ASSERT_TRUE(lua.available());
        const auto result = lua.run(
            "local loader = assert(package.loadlib([[" AI_ORACLE_LUA_PATH "]], 'luaopen_ai_oracle')); "
            "package.preload.ai_oracle = loader");
        ASSERT_TRUE(result.ok) << result.error;
#else
        GTEST_SKIP() << "ai-oracle Lua module unavailable";
#endif
    }
    void run(std::string_view source) {
        const auto result = lua.run(source, "ai_oracle_integration");
        EXPECT_TRUE(result.ok) << result.error;
    }
};
}  // namespace

TEST(AiOracleIntegration, CrawlerSessionSnapshotUsesHostTransportWithoutSiteCredentials) {
    prowsetk::BrowserConfig config;
    config.javascript = false;
    prowsetk::Browser browser(config);
    auto network = std::make_unique<prowsetk::MemoryNetworkClient>();
    auto* transport = network.get();
    network->set_handler([](const auto& http) {
        EXPECT_EQ(http.url, "https://api.openai.com/v1/responses");
        for (const auto& [name, value] : http.headers) {
            EXPECT_NE(name, "Cookie");
            EXPECT_NE(value, "site-private-fixture");
        }
        prowsetk::HttpResponse response;
        response.status = 200;
        response.body = answer(R"({"next":"/catalog","reason":"relevant link"})");
        return response;
    });
    browser.set_network_client(std::move(network));
    auto session = browser.create_session();
    session->set_header("Authorization", "site-private-fixture");
    session->load_html("<a href='/catalog'>Catalog</a><input name='password' value='site-private-fixture'>", "https://example.test/");
    ai::OracleOptions options;
    options.enabled = true;
    options.api_key = "fixture-key";
    ai::Oracle oracle(browser.network_client(), options);
    const auto result = oracle.ask_document(*session->document(), "Choose a link", ai::Task::Crawl, true);
    EXPECT_EQ(Json::parse(result.answer)["next"], "/catalog");
    EXPECT_TRUE(result.advisory);
    EXPECT_EQ(session->current_url(), "https://example.test/");
    ASSERT_EQ(transport->requests().size(), 1u);
    EXPECT_EQ(transport->requests()[0].body.find("site-private-fixture"), std::string::npos);
}

TEST_F(AiOracleLua, ExplicitEnableAndCredentialsAreRequiredBeforeSending) {
    run(R"LUA(
        local ai = require('ai_oracle')
        local calls = 0
        local function send() calls = calls + 1; error('must not send') end
        local disabled = assert(ai.new({api_key='fixture-key'}, send))
        local result, err, code = disabled:ask({prompt='Inspect'})
        assert(result == nil and err:find('disabled') and code == 'security_violation')
        local missing = assert(ai.new({enabled=true, api_key=''}, send))
        result, err, code = missing:ask({prompt='Inspect'})
        assert(result == nil and err:find('API key') and code == 'invalid_argument')
        assert(calls == 0 and disabled:request_count() == 0)
        assert(disabled:close() and disabled:close())
        result, err = disabled:ask({prompt='Inspect'})
        assert(result == nil and err:find('closed'))
    )LUA");
}

TEST_F(AiOracleLua, CaptchaImageAndStructuredCrawlHelpersUseRealNativeCodec) {
    run(R"LUA(
        local ai = require('ai_oracle')
        local calls = 0
        local oracle = assert(ai.new({enabled=true, api_key='fixture-key', timeout_ms=1234, max_response_bytes=4096},
            function(request)
                calls = calls + 1
                assert(request.method == 'POST' and request.url == 'https://api.openai.com/v1/responses')
                assert(request.timeout_ms == 1234 and request.max_response_bytes == 4096)
                assert(request.headers.Authorization == 'Bearer fixture-key' and request.headers.Cookie == nil)
                assert(not request.body:find('private%-fixture'))
                if calls == 1 then
                    assert(request.body:find('input_image', 1, true))
                    assert(request.body:find('data:image/png;base64,aW1hZ2U=', 1, true))
                else
                    assert(request.body:find('json_object', 1, true))
                end
                return {status=200, body=[[{"id":"resp_fixture","model":"fixture-model","status":"completed",
                    "output":[{"type":"message","role":"assistant","status":"completed",
                      "content":[{"type":"output_text","text":"{\"next\":\"/catalog\"}"}]}]}]]}
            end))
        local result = assert(oracle:captcha({prompt='Read the challenge image', images={'data:image/png;base64,aW1hZ2U='},
            context_json='{"password":"private-fixture"}', html='<input value="private-fixture">'}))
        assert(result.advisory and result.provenance == 'openai-responses' and result.response_id == 'resp_fixture')
        result = assert(oracle:crawl({prompt='Choose the next link'}))
        assert(result.answer == '{"next":"/catalog"}')
        assert(oracle:request_count() == 2)
        assert(oracle:close())
        collectgarbage('collect')
    )LUA");
}

TEST_F(AiOracleLua, ComposesWithManagedSessionAndHostNetworkHooks) {
    prowsetk::BrowserConfig config;
    config.javascript = false;
    config.follow_redirects = false;
    config.max_response_bytes = 4096;
    bound_browser = std::make_unique<prowsetk::Browser>(config);
    auto& browser = *bound_browser;
    auto network = std::make_unique<prowsetk::MemoryNetworkClient>();
    auto* transport = network.get();
    network->set_handler([](const auto&) {
        prowsetk::HttpResponse response;
        response.status = 200;
        response.body = answer(R"({"next":"/catalog"})");
        return response;
    });
    browser.set_network_client(std::move(network));
    lua.bind_browser(&browser);
    run(R"LUA(
        local ai = require('ai_oracle')
        local prowse = require('lprowse')
        local api_session = prowse.browser.new():create_session()
        local hooks = 0
        api_session:on('before_request', function() hooks = hooks + 1 end)
        local oracle = assert(ai.new({enabled=true, api_key='fixture-key', max_response_bytes=4096}, function(request)
            return api_session:request(request.method, request.url, {headers=request.headers, body=request.body})
        end))
        local result = assert(oracle:crawl({prompt='Choose the next link', html='<a href="/catalog">Catalog</a>'}))
        assert(result.advisory and result.answer == '{"next":"/catalog"}')
        assert(hooks == 1 and api_session:current_url() == '')
        assert(oracle:close())
    )LUA");
    ASSERT_EQ(transport->requests().size(), 1u);
    EXPECT_EQ(transport->requests()[0].url, "https://api.openai.com/v1/responses");
}

TEST_F(AiOracleLua, TransportFailuresArePrivateAndConsumeFiniteBudget) {
    run(R"LUA(
        local ai = require('ai_oracle')
        local calls = 0
        local oracle = assert(ai.new({enabled=true, api_key='fixture-key', max_requests=1}, function()
            calls = calls + 1
            error('private-fixture fixture-key')
        end))
        local result, err, code = oracle:ask({prompt='Inspect'})
        assert(result == nil and code == 'network_error' and not err:find('private%-fixture') and not err:find('fixture%-key'))
        result, err, code = oracle:ask({prompt='Inspect'})
        assert(result == nil and code == 'resource_limit' and calls == 1)
        assert(oracle:request_count() == 1)
    )LUA");
}

TEST_F(AiOracleLua, StrictOptionsAndInputBoundsFailBeforeCallback) {
    run(R"LUA(
        local ai = require('ai_oracle')
        for _, options in ipairs({{enabled='true'}, {timeout_ms=-1}, {timeout_ms=0}, {max_requests=1.5},
            {max_input_bytes=99999999}, {base_url='http://example.test/v1'}}) do
            local value, err = ai.new(options)
            assert(value == nil and type(err) == 'string')
        end
        local calls = 0
        local oracle = assert(ai.new({enabled=true, api_key='fixture-key', max_input_bytes=64}, function()
            calls = calls + 1
        end))
        local value, err, code = oracle:ask({prompt=string.rep('x', 65)})
        assert(value == nil and code == 'resource_limit' and calls == 0)
        value, err = oracle:ask({prompt='Inspect', context_json='not-json'})
        assert(value == nil and not err:find('not%-json') and calls == 0)
        value, err = oracle:captcha({prompt='Inspect', images={'https://example.test/a.png'}})
        assert(value == nil and calls == 0)
    )LUA");
}

TEST_F(AiOracleLua, CallbackCannotCloseOrReenterActiveClient) {
    run(R"LUA(
        local ai = require('ai_oracle')
        local oracle
        oracle = assert(ai.new({enabled=true, api_key='fixture-key'}, function()
            local result, err = oracle:close()
            assert(result == nil and err:find('busy'))
            result, err = oracle:ask({prompt='recursive'})
            assert(result == nil and err:find('busy'))
            return {status=200, body=[[{"id":"resp_fixture","model":"fixture-model","status":"completed",
                "output":[{"type":"message","role":"assistant","status":"completed",
                  "content":[{"type":"output_text","text":"inspect catalog"}]}]}]]}
        end))
        assert(oracle:ask({prompt='Inspect'}).answer == 'inspect catalog')
        assert(oracle:request_count() == 1)
        assert(oracle:close())
    )LUA");
}

TEST_F(AiOracleLua, CallbackCyclesAreCollectedAndCoroutinesUseTheirOwnStack) {
    run(R"LUA(
        local ai = require('ai_oracle')
        local weak = setmetatable({}, {__mode='v'})
        local function create()
            local oracle
            local marker = {}
            oracle = assert(ai.new({enabled=true, api_key='fixture-key'}, function(request)
                assert(oracle:request_count() == 1 and marker ~= nil)
                assert(request.method == 'POST')
                return {status=200, body=[[{"id":"resp_fixture","model":"fixture-model","status":"completed",
                    "output":[{"type":"message","role":"assistant","status":"completed",
                      "content":[{"type":"output_text","text":"inspect catalog"}]}]}]]}
            end))
            weak[1], weak[2] = oracle, marker
            return oracle
        end
        do
            local oracle = create()
            local co = coroutine.create(function()
                assert(oracle:ask({prompt='Inspect'}).answer == 'inspect catalog')
            end)
            assert(coroutine.resume(co))
        end
        collectgarbage('collect')
        collectgarbage('collect')
        assert(weak[1] == nil and weak[2] == nil)
    )LUA");
}

TEST(AiOracleIntegration, SharedPluginExportsCServiceWithoutCppTypes) {
#if defined(__unix__) || defined(__APPLE__)
    void* library = dlopen(AI_ORACLE_PLUGIN_PATH, RTLD_NOW | RTLD_LOCAL);
    ASSERT_NE(library, nullptr);
    for (const auto* symbol : {"prowsetk_ai_oracle_create", "prowsetk_ai_oracle_ask", "prowsetk_ai_oracle_free",
                              "prowsetk_ai_oracle_options_init", "prowsetk_ai_oracle_error"}) {
        EXPECT_NE(dlsym(library, symbol), nullptr) << symbol;
    }
    dlclose(library);
#else
    GTEST_SKIP() << "native plugin loading unavailable";
#endif
}
