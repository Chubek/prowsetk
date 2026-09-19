#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "prowsetk/document.hpp"
#include "prowsetk/flatworm_host.hpp"
#include "prowsetk/javascript_runtime.hpp"

using prowsetk::DocumentScriptHost;
using prowsetk::HostRequest;
using prowsetk::HostResponse;
using prowsetk::make_detached_script_host;
using prowsetk::make_javascript_runtime;
using prowsetk::parse_html;
using prowsetk::ScriptResult;

namespace {

struct Platform {
    std::unique_ptr<prowsetk::FlatwormScriptHost> host;
    std::unique_ptr<prowsetk::JavaScriptRuntime> runtime;
    std::vector<std::pair<std::string, std::string>> requests;

    ScriptResult run(const std::string& script) { return runtime->evaluate(script); }
    std::string value(const std::string& expression) {
        auto result = runtime->evaluate(expression);
        return result.ok ? result.value : "<error:" + result.error + ">";
    }
    void flush() {
        for (int pass = 0; pass < 16; ++pass) {
            auto result = runtime->evaluate("__prowsetkFlush();");
            if (!result.ok || result.value == "0") break;
        }
    }
};

Platform make_platform(const std::string& html =
                           "<html><head><title>T</title></head>"
                           "<body><div id='mount' class='a b'>hi</div>"
                           "<p data-role='x'>one</p><p>two</p></body></html>") {
    Platform platform;
    platform.host = make_detached_script_host(parse_html(html, "https://app.test/page?q=1#frag", "https://app.test/page?q=1#frag"));
    platform.host->set_request_hook(
        [&requests = platform.requests](const HostRequest& request) {
            HostResponse response;
            requests.emplace_back(request.method, request.url);
            if (request.url == "https://app.test/api/form") {
                response.ok = true;
                response.status = 200;
                response.body = "<form method='post' action='/login'>"
                                "<input name='q'></form>";
                response.headers.emplace_back("Content-Type", "text/html");
                response.final_url = request.url;
                return response;
            }
            response.ok = true;
            response.status = 204;
            response.final_url = request.url;
            return response;
        });
    platform.runtime = make_javascript_runtime(platform.host.get());
    return platform;
}

}  // namespace

TEST(JavaScriptPlatform, RuntimeAdvertisesGlobalsAndAliases) {
    if (make_javascript_runtime()->name() == "null") GTEST_SKIP();
    auto platform = make_platform();
    EXPECT_EQ(platform.value("typeof document"), "object");
    EXPECT_EQ(platform.value("typeof XMLHttpRequest"), "function");
    EXPECT_EQ(platform.value("typeof fetch"), "function");
    EXPECT_EQ(platform.value("typeof setTimeout"), "function");
    EXPECT_EQ(platform.value("window === self"), "true");
    EXPECT_EQ(platform.value("typeof navigator.userAgent"), "string");
}

TEST(JavaScriptPlatform, DocumentQueriesAndTraversal) {
    auto platform = make_platform();
    EXPECT_EQ(platform.value("document.querySelectorAll('p').length"), "2");
    EXPECT_EQ(platform.value("document.querySelector('#mount').id"), "mount");
    EXPECT_EQ(platform.value("document.getElementById('mount').className"), "a b");
    EXPECT_EQ(platform.value("document.getElementById('nope')"), "null");
    EXPECT_EQ(platform.value("document.getElementsByTagName('p')[0].textContent"),
              "one");
    EXPECT_EQ(platform.value("document.querySelector('p').matches('p[data-role]')"),
              "true");
    EXPECT_EQ(platform.value(
                  "document.querySelector('p').nextElementSibling.textContent"),
              "two");
    EXPECT_EQ(platform.value("document.body.tagName"), "BODY");
    EXPECT_EQ(platform.value("document.title"), "T");
    EXPECT_EQ(platform.value("document.location.href"),
              "https://app.test/page?q=1#frag");
}

TEST(JavaScriptPlatform, ElementMutationReachesDocument) {
    auto platform = make_platform();
    ASSERT_TRUE(platform
                    .run("var el = document.createElement('section');"
                         "el.id = 'made'; el.className = 'c';"
                         "el.textContent = 'built';"
                         "document.body.appendChild(el);")
                    .ok);
    EXPECT_EQ(platform.value("document.getElementById('made').textContent"),
              "built");
    auto document = platform.host->document();
    ASSERT_NE(document, nullptr);
    auto made = document->query_selector("#made.c");
    ASSERT_NE(made, nullptr);
    EXPECT_EQ(made->text(), "built");

    ASSERT_TRUE(platform.run("document.querySelector('#mount').innerHTML ="
                             " '<em>inner</em>';").ok);
    EXPECT_EQ(platform.value("document.querySelector('#mount em').textContent"),
              "inner");
    ASSERT_TRUE(platform.run("var removed = document.querySelector('#mount');"
                             "removed.remove();").ok);
    EXPECT_EQ(platform.value("document.querySelector('#mount')"), "null");
}

TEST(JavaScriptPlatform, ClassListDatasetAndStyle) {
    auto platform = make_platform();
    ASSERT_TRUE(platform.run("document.getElementById('mount').classList.remove('a');")
                    .ok);
    EXPECT_EQ(platform.value("document.getElementById('mount').className"), "b");
    EXPECT_EQ(platform.value("document.getElementById('mount')"
                             ".classList.toggle('z')"), "true");
    EXPECT_EQ(platform.value("document.querySelector('p').dataset.role"), "x");
    ASSERT_TRUE(platform.run("document.body.style.display = 'none';"
                             "document.body.setAttribute('style',"
                             " document.body.getAttribute('style') + '; opacity: 0.5');")
                    .ok);
    auto body = platform.host->document()->query_selector("body");
    ASSERT_NE(body, nullptr);
    EXPECT_NE(body->attribute("style").find("display: none"), std::string::npos);
    EXPECT_EQ(platform.value("document.body.style.getPropertyValue('opacity')"),
              "0.5");
}

TEST(JavaScriptPlatform, SynchronousXhrMountsResponse) {
    auto platform = make_platform();
    ASSERT_TRUE(platform
                    .run("var x = new XMLHttpRequest();"
                         "x.open('GET', '/api/form', false);"
                         "x.send();"
                         "globalThis.xhrStatus = x.status;"
                         "document.getElementById('mount').innerHTML = x.responseText;")
                    .ok);
    EXPECT_EQ(platform.value("xhrStatus"), "200");
    EXPECT_EQ(platform.value("document.querySelector('#mount form').id || 'anon'"),
              "anon");
    auto form = platform.host->document()->query_selector("#mount form");
    ASSERT_NE(form, nullptr);
    EXPECT_EQ(form->attribute("action"), "/login");
    ASSERT_EQ(platform.requests.size(), 1u);
    EXPECT_EQ(platform.requests[0].second, "https://app.test/api/form");
}

TEST(JavaScriptPlatform, AsynchronousXhrAndFetchDeliverThroughJobs) {
    auto platform = make_platform();
    ASSERT_TRUE(platform
                    .run("globalThis.loaded = 'pending';"
                         "var x = new XMLHttpRequest();"
                         "x.open('GET', '/api/form', true);"
                         "x.onload = function () { loaded = x.status; };"
                         "x.send();")
                    .ok);
    // Async callbacks are delivered through microtasks drained by evaluate.
    EXPECT_EQ(platform.value("loaded"), "200");

    ASSERT_TRUE(platform
                    .run("globalThis.fetchJson = 'pending';"
                         "fetch('/api/form').then(function (r) {"
                         "  globalThis.fetchJson = r.status + ':' + r.headers.get('Content-Type');"
                         "});")
                    .ok);
    EXPECT_EQ(platform.value("fetchJson"), "200:text/html");
    EXPECT_GE(platform.requests.size(), 2u);
}

TEST(JavaScriptPlatform, TimersRunInFlushPasses) {
    auto platform = make_platform();
    ASSERT_TRUE(platform
                    .run("setTimeout(function () {"
                         "  document.getElementById('mount').textContent = 'timed';"
                         "}, 0);"
                         "var intervalCount = 0;"
                         "var handle = setInterval(function () {"
                         "  intervalCount = intervalCount + 1;"
                         "  if (intervalCount > 2) clearInterval(handle);"
                         "}, 0);")
                    .ok);
    EXPECT_EQ(platform.value("document.getElementById('mount').textContent"), "hi");
    platform.flush();
    EXPECT_EQ(platform.value("document.getElementById('mount').textContent"),
              "timed");
    EXPECT_EQ(platform.value("intervalCount"), "3");
}

TEST(JavaScriptPlatform, StorageCookiesAndPageNavigationRequests) {
    auto platform = make_platform();
    ASSERT_TRUE(platform
                    .run("localStorage.setItem('theme', 'dark');"
                         "sessionStorage.setItem('s', '1');"
                         "globalThis.got = localStorage.getItem('theme');"
                         "globalThis.missing = localStorage.getItem('nope');"
                         "globalThis.len = localStorage.length;"
                         "document.cookie = 'sid=abc';"
                         "location.assign('/next');")
                    .ok);
    EXPECT_EQ(platform.value("got"), "dark");
    EXPECT_EQ(platform.value("missing"), "null");
    EXPECT_EQ(platform.value("len"), "1");
    EXPECT_EQ(platform.host->storage_item("local", "theme"), "dark");
    EXPECT_EQ(platform.host->cookie_header(), "");  // no jar in a detached host
    prowsetk::PendingNavigation navigation;
    ASSERT_TRUE(platform.host->consume_pending_navigation(navigation));
    EXPECT_EQ(navigation.url, "https://app.test/next");
    EXPECT_EQ(navigation.method, "GET");
}

TEST(JavaScriptPlatform, EventsAndFormSubmit) {
    auto platform = make_platform(
        "<html><body><form id='f' action='/search'>"
        "<input name='q' value='hello'></form>"
        "<a id='link' href='/away'>go</a></body></html>");
    ASSERT_TRUE(platform
                    .run("globalThis.clicked = 0;"
                         "document.getElementById('link').addEventListener('click',"
                         " function () { clicked = clicked + 1; });"
                         "document.getElementById('link').click();")
                    .ok);
    EXPECT_EQ(platform.value("clicked"), "1");
    prowsetk::PendingNavigation navigation;
    ASSERT_TRUE(platform.host->consume_pending_navigation(navigation));
    EXPECT_EQ(navigation.url, "https://app.test/away");

    ASSERT_TRUE(platform.run("document.getElementById('f').submit();").ok);
    ASSERT_TRUE(platform.host->consume_pending_navigation(navigation));
    EXPECT_EQ(navigation.url, "https://app.test/search?q=hello");
}

TEST(JavaScriptPlatform, UrlHelpersAndBase64AndParserStubs) {
    auto platform = make_platform();
    EXPECT_EQ(platform.value("new URL('/deep/x?a=1&b=2', location.origin).href"),
              "https://app.test/deep/x?a=1&b=2");
    EXPECT_EQ(platform.value("new URLSearchParams('a=1&b=2').get('b')"), "2");
    EXPECT_EQ(platform.value("atob(btoa('hello'))"), "hello");
    EXPECT_EQ(platform.value("btoa('hi')"), "aGk=");
    EXPECT_EQ(platform.value("atob('aGk=')"), "hi");
    ASSERT_TRUE(platform
                    .run("var dom = new DOMParser();"
                         "var parsed = dom.parseFromString('<i>yes</i>', 'text/html');"
                         "globalThis.parsedText = parsed.querySelector('i').textContent;")
                    .ok);
    EXPECT_EQ(platform.value("parsedText"), "yes");
    // Observer and media stubs are constructible and inert.
    EXPECT_TRUE(platform
                    .run("new MutationObserver(function () {})"
                         "    .observe(document.body, {childList: true});"
                         "globalThis.mm = matchMedia('(min-width: 1px)').matches;"
                         "getComputedStyle(document.body).getPropertyValue('display');")
                        .ok);
    EXPECT_EQ(platform.value("mm"), "false");
}

TEST(JavaScriptPlatform, ScriptErrorsAreReportedNotFatal) {
    auto platform = make_platform();
    const auto result = platform.run("document.getElementById('mount').nope();");
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("not a function"), std::string::npos)
        << result.error;
    // The runtime keeps working after an exception.
    EXPECT_EQ(platform.value("1 + 1"), "2");
}

TEST(JavaScriptPlatform, LifecycleEventsFireOnceInOrder) {
    auto platform = make_platform();
    ASSERT_TRUE(platform
                    .run("globalThis.order = [];"
                         "document.addEventListener('DOMContentLoaded',"
                         " function () { order.push('dcl:' + document.readyState); });"
                         "window.addEventListener('load',"
                         " function () { order.push('load:' + document.readyState); });")
                    .ok);
    platform.flush();
    platform.flush();  // a second flush must not re-fire lifecycle events
    EXPECT_EQ(platform.value("order.join(',')"), "dcl:interactive,load:complete");
}
