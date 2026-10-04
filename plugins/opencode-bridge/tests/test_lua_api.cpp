// Lua integration tests for `lopencode` via package.loadlib.
// Hermetic: the client transport is a Lua callback, never a live server.

#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include "prowsetk/lua_runtime.hpp"

namespace {

class LopencodeLua : public testing::Test {
protected:
    prowsetk::LuaRuntime lua;
    void SetUp() override {
#ifdef LOPENCODE_LUA_PATH
        ASSERT_TRUE(prowsetk::LuaRuntime::available());
        const auto result = lua.run(
            "local loader = assert(package.loadlib([[" LOPENCODE_LUA_PATH "]], 'luaopen_lopencode')); "
            "package.preload.lopencode = loader");
        ASSERT_TRUE(result.ok) << result.error;
#else
        GTEST_SKIP() << "lopencode Lua module unavailable";
#endif
    }
    void run(std::string_view source) {
        const auto result = lua.run(source, "lopencode_integration");
        EXPECT_TRUE(result.ok) << result.error;
    }
};

constexpr const char* kTransport = R"LUA(
    local function transport(request)
        assert(request.method == 'POST')
        assert(request.url:find('127.0.0.1:4096', 1, true))
        if request.headers.Authorization ~= nil then
            assert(request.headers.Authorization:find('Basic ', 1, true) == 1)
        end
        if request.url:find('/session', 1, true) and not request.url:find('/message', 1, true) then
            return {status=200, body='{"id":"sess-lua"}'}
        end
        return {status=200, body='{"text":"{\\"title\\":\\"Hi\\"}"}'}
    end
)LUA";

}  // namespace

TEST_F(LopencodeLua, ClientNewExposesPromptWorkflow) {
    run(std::string(kTransport) + R"LUA(
        local lopencode = require('lopencode')
        local client = assert(lopencode.client.new(
            {base_url='http://127.0.0.1:4096', username='u', password='p'}, transport))
        local id = assert(client:create_session())
        assert(id == 'sess-lua')
        local answer = assert(client:prompt(id, 'Summarize'))
        assert(answer:find('title'))
        assert(client:request_count() == 2)
        assert(client:close())
    )LUA");
}

TEST_F(LopencodeLua, PromptAsyncAbortAndStreamEvents) {
    run(R"LUA(
        local lopencode = require('lopencode')
        local function transport(request)
            if request.url:find('prompt_async', 1, true) then
                return {status=200, body='{"operation_id":"op-1"}'}
            elseif request.url:find('/event', 1, true) then
                assert(request.method == 'GET')
                return {status=200, body='data: {"t":1}\n\ndata: {"t":2}\n'}
            elseif request.url:find('abort', 1, true) then
                return {status=200, body='{}'}
            end
            return {status=200, body='{"id":"s"}'}
        end
        local client = assert(lopencode.client.new({base_url='http://127.0.0.1:4096'}, transport))
        assert(client:prompt_async('s', 'crawl') == 'op-1')
        local seen = {}
        local count = assert(client:stream_events('s', function(event) seen[#seen + 1] = event end))
        assert(count == 2 and seen[1] == '{"t":1}' and seen[2] == '{"t":2}')
        assert(client:abort('s'))
        assert(client:close())
    )LUA");
}

TEST_F(LopencodeLua, ScrapeWithPromptUsesSnapshotTable) {
    run(std::string(kTransport) + R"LUA(
        local lopencode = require('lopencode')
        local client = assert(lopencode.new(
            {base_url='http://127.0.0.1:4096', username='u', password='p'}, transport))
        local snapshot = {url='https://example.test/', title='Hi',
            html='<main><h1>Hi</h1><script>evil()</script></main>', text='Hi'}
        local answer = assert(client:scrape_with_prompt(snapshot, 'Get heading', '{"title":"string"}'))
        assert(answer:find('title'))
        assert(client:close())
    )LUA");
}

TEST_F(LopencodeLua, ScrapeWithPromptReadsLprowseSession) {
    run(std::string(kTransport) + R"LUA(
        local lopencode = require('lopencode')
        local prowse = require('lprowse')
        local browser = prowse.browser.new()
        local session = browser:create_session()
        session:load_html('<main><h1>Hi</h1></main>', 'https://example.test/')
        local client = assert(lopencode.client.new({base_url='http://127.0.0.1:4096'}, transport))
        local answer = assert(client:scrape_with_prompt(session, 'Get heading'))
        assert(answer:find('title'))
    )LUA");
}

TEST_F(LopencodeLua, TransportFailuresAreSecretFreeAndBounded) {
    run(R"LUA(
        local lopencode = require('lopencode')
        local calls = 0
        local client = assert(lopencode.client.new(
            {base_url='http://127.0.0.1:4096', max_requests=1}, function()
                calls = calls + 1
                error('private-fixture')
            end))
        local answer, err, code = client:create_session()
        assert(answer == nil and code == 'network_error')
        assert(not err:find('private%-fixture'))
        answer, err, code = client:create_session()
        assert(answer == nil and code == 'resource_limit' and calls == 1)
    )LUA");
}

TEST_F(LopencodeLua, CallbackCannotCloseOrReenterActiveClient) {
    run(R"LUA(
        local lopencode = require('lopencode')
        local client
        client = assert(lopencode.client.new({base_url='http://127.0.0.1:4096'}, function()
            local result, err = client:close()
            assert(result == nil and err:find('busy'))
            result, err = client:prompt('s', 'recursive')
            assert(result == nil and err:find('busy'))
            return {status=200, body='{"id":"s"}'}
        end))
        assert(client:create_session() == 's')
        assert(client:request_count() == 1)
        assert(client:close())
    )LUA");
}

TEST_F(LopencodeLua, CallbackCyclesAreCollected) {
    run(R"LUA(
        local lopencode = require('lopencode')
        local weak = setmetatable({}, {__mode='v'})
        local function create()
            local client
            local marker = {}
            client = assert(lopencode.client.new({base_url='http://127.0.0.1:4096'}, function(request)
                assert(client:request_count() == 1 and marker ~= nil)
                return {status=200, body='{"id":"s"}'}
            end))
            weak[1], weak[2] = client, marker
            return client
        end
        do
            local client = create()
            assert(client:create_session() == 's')
        end
        collectgarbage('collect')
        collectgarbage('collect')
        assert(weak[1] == nil and weak[2] == nil)
    )LUA");
}

TEST_F(LopencodeLua, BuildCleanupPromptIsPureAndBounded) {
    run(R"LUA(
        local lopencode = require('lopencode')
        local prompt = assert(lopencode.build_cleanup_prompt(
            '[{"url":"https://a.test/api","method":"get"}]', 'prefer v2'))
        assert(prompt:find('NEVER invent', 1, true))
        assert(prompt:find('https://a.test/api', 1, true))
        local value, err = lopencode.build_cleanup_prompt('')
        assert(value == nil and type(err) == 'string')
    )LUA");
}

TEST_F(LopencodeLua, V2ApiPromptWaitsForAgentReply) {
    run(R"LUA(
        local lopencode = require('lopencode')
        local calls = 0
        local client = assert(lopencode.client.new(
            {base_url='http://127.0.0.1:4096', api_prefix='/api', prompt_wait_ms=5000},
            function(request)
                calls = calls + 1
                if calls == 1 then
                    assert(request.method == 'POST' and request.url:find('/api/session', 1, true))
                    return {status=200, body='{"data":{"id":"ses_v2"}}'}
                end
                if request.url:find('/prompt', 1, true) then
                    assert(request.method == 'POST')
                    return {status=200, body='{"data":{"id":"msg_1"}}'}
                end
                assert(request.method == 'GET' and request.url:find('/message', 1, true))
                if calls == 2 then
                    return {status=200, body='{"data":[]}'}
                end
                return {status=200, body='{"data":[{"id":"msg_2","type":"assistant",'
                    .. '"time":{"created":1,"completed":2},"content":['
                    .. '{"type":"reasoning","text":"thinking"},'
                    .. '{"type":"text","text":"{\\"kept\\":true}"}]}]}'}
            end))
        local session = assert(client:create_session())
        assert(session == 'ses_v2')
        local answer = assert(client:prompt(session, 'clean'))
        assert(answer == '{"kept":true}')
        assert(calls == 4)
        assert(client:close())
    )LUA");
}

TEST_F(LopencodeLua, V2ApiOptionsAreValidated) {
    run(R"LUA(
        local lopencode = require('lopencode')
        local value, err = lopencode.new({base_url='http://127.0.0.1:4096', api_prefix='/v9'})
        assert(value == nil and type(err) == 'string')
        value, err = lopencode.new({base_url='http://127.0.0.1:4096', prompt_wait_ms=999999})
        assert(value == nil and type(err) == 'string')
    )LUA");
}

TEST_F(LopencodeLua, RemoteHttpRequiresExplicitOptIn) {
    run(R"LUA(
        local lopencode = require('lopencode')
        local value, err = lopencode.new({base_url='http://opencode.lan:4096'})
        assert(value == nil and type(err) == 'string')
        local client = assert(lopencode.new(
            {base_url='http://opencode.lan:4096', allow_remote_http=true}, function()
                return {status=200, body='{"id":"s"}'}
            end))
        assert(client:create_session() == 's')
        assert(client:close())
    )LUA");
}

TEST_F(LopencodeLua, StrictOptionsFailBeforeTransport) {
    run(R"LUA(
        local lopencode = require('lopencode')
        for _, options in ipairs({{base_url='notaurl'}, {timeout_ms=-1},
                {max_requests=1.5}, {max_input_bytes=99999999}}) do
            local value, err = lopencode.new(options)
            assert(value == nil and type(err) == 'string')
        end
        local calls = 0
        local client = assert(lopencode.new(
            {base_url='http://127.0.0.1:4096', max_input_bytes=64}, function()
                calls = calls + 1
            end))
        local value, err, code = client:prompt('s', string.rep('x', 65))
        assert(value == nil and code == 'resource_limit' and calls == 0)
    )LUA");
}
