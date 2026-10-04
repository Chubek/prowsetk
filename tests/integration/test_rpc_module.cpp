#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "prowsetk/browser.hpp"
#include "prowsetk/error.hpp"
#include "prowsetk/lua_runtime.hpp"

namespace {

prowsetk::HttpResponse reply(std::string body, int status = 200) {
    prowsetk::HttpResponse response;
    response.status = status;
    response.headers.emplace_back("Content-Type", "application/json");
    response.body = std::move(body);
    return response;
}

std::string header(const prowsetk::HttpRequest& request, std::string_view name) {
    prowsetk::HttpResponse fields;
    fields.headers = request.headers;
    return fields.header(name);
}

class RpcSessions : public ::testing::Test {
protected:
    void SetUp() override {
        if (prowsetk::make_javascript_runtime()->name() == "null") GTEST_SKIP();
        auto transport = std::make_unique<prowsetk::MemoryNetworkClient>();
        network = transport.get();
        browser.set_network_client(std::move(transport));
        browser.modules().load_native(RPC_MODULE_PATH);
        browser.events().subscribe(prowsetk::EventType::ScriptException,
            [this](const prowsetk::Event& event) { failures.push_back(event.message); });
    }
    prowsetk::Browser browser;
    prowsetk::MemoryNetworkClient* network = nullptr;
    std::vector<std::string> failures;
};

TEST_F(RpcSessions, ClassicStaticAndDynamicImportsShareAnIsolatedPersistentRpcInstance) {
    auto first = browser.create_session();
    auto second = browser.create_session();
    first->load_html(R"html(
        <p id="ids"></p>
        <script>document.getElementById('ids').textContent = Flatworm.module('rpc').request('a').id;</script>
        <script type="module">
            import {request} from 'flatworm:rpc';
            document.getElementById('ids').textContent += ':' + request('b').id;
            import('flatworm:rpc').then(rpc => document.getElementById('ids').textContent += ':' + rpc.request('c').id);
        </script>
    )html", "https://example.test/");
    ASSERT_TRUE(failures.empty());
    EXPECT_EQ(first->document()->query_selector("#ids")->text(), "1:2:3");
    EXPECT_EQ(second->evaluate_js("Flatworm.module('rpc').request('a').id"), "1");
    first->load_html("<p>new page</p><script>document.querySelector('p').textContent = Flatworm.module('rpc').request('d').id</script>");
    EXPECT_EQ(first->document()->query_selector("p")->text(), "4");
    EXPECT_TRUE(browser.modules().remove("rpc"));
    EXPECT_EQ(first->evaluate_js("Flatworm.module('rpc').request('e').id"), "5");
    auto unselected = browser.create_session();
    EXPECT_THROW(unselected->evaluate_js("Flatworm.module('rpc')"), prowsetk::Error);
    first->close();
    second->close();
    browser.modules().load_native(RPC_MODULE_PATH);
    auto fresh = browser.create_session();
    EXPECT_EQ(fresh->evaluate_js("Flatworm.module('rpc').request('fresh').id"), "1");
    EXPECT_TRUE(network->requests().empty());
    EXPECT_TRUE(browser.plugins().plugins().empty());
}

TEST_F(RpcSessions, FetchUsesSessionHeadersCookiesRedirectsAndObservableHostRequests) {
    auto redirect = reply("", 307);
    redirect.headers.emplace_back("Location", "/rpc-confirmed");
    network->set_response("https://example.test/rpc", std::move(redirect));
    network->set_response("https://example.test/rpc-confirmed", reply(R"({"jsonrpc":"2.0","id":1,"result":42})"));
    std::vector<prowsetk::Event> events;
    for (const auto type : {prowsetk::EventType::BeforeRequest, prowsetk::EventType::AfterResponse,
                            prowsetk::EventType::BeforeRedirect}) {
        browser.events().subscribe(type, [&](const prowsetk::Event& event) { events.push_back(event); });
    }
    auto session = browser.create_session();
    session->set_header("Authorization", "Bearer private-rpc-auth-marker");
    session->load_html(R"html(
        <p id="answer"></p>
        <script type="module">
            import {request, parseResponse} from 'flatworm:rpc';
            document.cookie = 'sid=private-rpc-cookie-marker; Path=/';
            const message = request('add', [20, 22]);
            const http = await fetch('/rpc', {
                method: 'POST', headers: {'Content-Type':'application/json'}, body: JSON.stringify(message)
            });
            if (!http.ok) throw new Error('RPC HTTP failure');
            const response = parseResponse(await http.text(), message.id);
            if (response.error) throw new Error('RPC remote failure');
            document.getElementById('answer').textContent = response.result;
        </script>
    )html", "https://example.test/");
    ASSERT_TRUE(failures.empty());
    EXPECT_EQ(session->document()->query_selector("#answer")->text(), "42");
    ASSERT_EQ(network->requests().size(), 2u);
    EXPECT_EQ(network->requests()[1].url, "https://example.test/rpc-confirmed");
    for (const auto& request : network->requests()) {
        EXPECT_EQ(request.method, "POST");
        EXPECT_EQ(request.body, R"({"jsonrpc":"2.0","method":"add","params":[20,22],"id":1})");
        EXPECT_EQ(header(request, "Content-Type"), "application/json");
        EXPECT_EQ(header(request, "Authorization"), "Bearer private-rpc-auth-marker");
        EXPECT_EQ(header(request, "Cookie"), "sid=private-rpc-cookie-marker");
    }
    EXPECT_EQ(events.size(), 5u);
    for (const auto& event : events) {
        EXPECT_EQ(event.message.find("private-rpc-"), std::string::npos);
        for (const auto& [key, value] : event.attributes) {
            EXPECT_EQ(key.find("private-rpc-"), std::string::npos);
            EXPECT_EQ(value.find("private-rpc-"), std::string::npos);
        }
    }
    ASSERT_EQ(session->page_script_requests().size(), 1u);
    EXPECT_EQ(session->page_script_requests()[0].method, "POST");
    EXPECT_EQ(session->page_script_requests()[0].url, "https://example.test/rpc");
}

TEST_F(RpcSessions, BatchFetchCorrelatesReorderedSuccessAndErrorRepliesWithoutNotificationReplies) {
    network->set_response("https://example.test/rpc", reply(
        R"([{"jsonrpc":"2.0","id":2,"error":{"code":-32601,"message":"Method not found"}},{"jsonrpc":"2.0","id":1,"result":3}])"));
    auto session = browser.create_session();
    session->load_html(R"html(
        <p id="answer"></p>
        <script type="module">
            import {request, notification, batch, correlate} from 'flatworm:rpc';
            const messages = batch([request('sum', [1,2]), notification('tick'), request('missing')]);
            const http = await fetch('/rpc', {method:'POST', body:JSON.stringify(messages)});
            const responses = correlate(messages, await http.text());
            document.getElementById('answer').textContent = responses[0].result + ':' + responses[1].error.code;
        </script>
    )html", "https://example.test/");
    ASSERT_TRUE(failures.empty());
    EXPECT_EQ(session->document()->query_selector("#answer")->text(), "3:-32601");
    ASSERT_EQ(network->requests().size(), 1u);
    EXPECT_EQ(network->requests()[0].body,
        R"([{"jsonrpc":"2.0","method":"sum","params":[1,2],"id":1},{"jsonrpc":"2.0","method":"tick"},{"jsonrpc":"2.0","method":"missing","id":2}])");
}

TEST_F(RpcSessions, NotificationAcceptsNoContentAndXhrUsesTheSameHostBoundary) {
    network->set_response("https://example.test/notify", reply("", 204));
    network->set_response("https://example.test/rpc", reply(R"({"jsonrpc":"2.0","id":"named","result":"ready"})"));
    auto session = browser.create_session();
    session->load_html(R"html(
        <p id="answer"></p>
        <script type="module">
            import {request, notification, correlate, parseResponse} from 'flatworm:rpc';
            const message = notification('tick');
            const http = await fetch('/notify', {method:'POST', body:JSON.stringify(message)});
            const none = correlate(message, await http.text());
            const call = request('status', {details:true}, 'named');
            const xhr = new XMLHttpRequest();
            xhr.open('POST', '/rpc', false);
            xhr.setRequestHeader('Content-Type', 'application/json');
            xhr.send(JSON.stringify(call));
            document.getElementById('answer').textContent = none.length + ':' + parseResponse(xhr.responseText, call.id).result;
        </script>
    )html", "https://example.test/");
    ASSERT_TRUE(failures.empty());
    EXPECT_EQ(session->document()->query_selector("#answer")->text(), "0:ready");
    ASSERT_EQ(network->requests().size(), 2u);
    EXPECT_EQ(network->requests()[0].body, R"({"jsonrpc":"2.0","method":"tick"})");
    EXPECT_EQ(network->requests()[1].body, R"({"jsonrpc":"2.0","method":"status","params":{"details":true},"id":"named"})");
    EXPECT_EQ(session->evaluate_js("Flatworm.module('rpc').request('next').id"), "1");
}

TEST_F(RpcSessions, CancellationPreventsRpcTransportAndMalformedRepliesEmitValueFreeErrors) {
    auto session = browser.create_session();
    session->load_html("<p>ready</p>", "https://example.test/");
    const auto cancellation = browser.events().subscribe(prowsetk::EventType::BeforeRequest,
        [](prowsetk::Event& event) { event.cancelled = true; });
    session->evaluate_js(R"js(
        fetch('/rpc', {method:'POST', body:JSON.stringify(Flatworm.module('rpc').request('x'))})
            .then(() => globalThis.blocked = false).catch(() => globalThis.blocked = true);
    )js");
    EXPECT_EQ(session->evaluate_js("blocked"), "true");
    EXPECT_TRUE(network->requests().empty());
    browser.events().unsubscribe(cancellation);
    network->set_response("https://example.test/rpc", reply("private-rpc-response-marker"));
    session->load_html(R"html(
        <p>retained</p>
        <script type="module">
            import {request, parseResponse} from 'flatworm:rpc';
            const message = request('x');
            const http = await fetch('/rpc', {method:'POST', body:JSON.stringify(message)});
            parseResponse(await http.text(), message.id);
        </script>
    )html", "https://example.test/");
    ASSERT_EQ(failures.size(), 1u);
    EXPECT_EQ(failures[0].find("private-rpc-response-marker"), std::string::npos);
    EXPECT_NE(failures[0].find("TypeError"), std::string::npos);
    EXPECT_EQ(session->document()->query_selector("p")->text(), "retained");
    EXPECT_EQ(session->evaluate_js("Flatworm.module('rpc').request('after').id"), "3");
}

TEST_F(RpcSessions, LuaDrivesTheHostSelectedModuleThroughManagedPageEvaluation) {
    if (!prowsetk::LuaRuntime::available()) GTEST_SKIP();
    network->set_response("https://example.test/rpc", reply(R"({"jsonrpc":"2.0","id":1,"result":42})"));
    auto session = browser.create_session();
    prowsetk::LuaRuntime lua;
    lua.bind_browser(&browser);
    lua.bind_session(session);
    const auto result = lua.run(R"lua(
        session:load_html('<p id="answer"></p>', 'https://example.test/')
        session:evaluate_js([[
            const rpc = Flatworm.module('rpc');
            const message = rpc.request('add', [20,22]);
            fetch('/rpc', {method:'POST', body:JSON.stringify(message)})
                .then(http => http.text()).then(body => {
                    document.getElementById('answer').textContent = rpc.parseResponse(body, message.id).result;
                });
        ]])
        assert(session:document():query_selector('#answer'):text() == '42')
    )lua");
    ASSERT_TRUE(result.ok) << result.error;
    ASSERT_TRUE(failures.empty());
    ASSERT_EQ(network->requests().size(), 1u);
    EXPECT_EQ(network->requests()[0].method, "POST");
}

TEST(RpcDisabledSessions, ModuleDefinitionsAreInertAndJavaScriptDisabledPagesMakeNoRpcRequests) {
    prowsetk::BrowserConfig config;
    config.javascript = false;
    prowsetk::Browser browser(config);
    EXPECT_TRUE(browser.modules().modules().empty());
    auto transport = std::make_unique<prowsetk::MemoryNetworkClient>();
    const auto* network = transport.get();
    browser.set_network_client(std::move(transport));
    const auto module = browser.modules().load_native(RPC_MODULE_PATH);
    EXPECT_EQ(module->info().name, "rpc");
    EXPECT_EQ(module->info().version, "1.0.0");
    EXPECT_EQ(module->info().exports.size(), 18u);
    auto session = browser.create_session();
    session->load_html("<p>offline</p><script>Flatworm.module('rpc').request('x'); fetch('/rpc');</script>");
    EXPECT_EQ(session->document()->query_selector("p")->text(), "offline");
    EXPECT_THROW(session->evaluate_js("Flatworm.module('rpc')"), prowsetk::Error);
    EXPECT_TRUE(network->requests().empty());
    EXPECT_TRUE(browser.plugins().plugins().empty());
}

}  // namespace
