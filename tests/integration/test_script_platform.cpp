#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "prowsetk/browser.hpp"
#include "prowsetk/event.hpp"
#include "prowsetk/network_client.hpp"

using prowsetk::Browser;
using prowsetk::Event;
using prowsetk::EventType;
using prowsetk::HttpRequest;
using prowsetk::HttpResponse;
using prowsetk::MemoryNetworkClient;

namespace {

HttpResponse html(std::string body, std::string url = {}) {
    HttpResponse response;
    response.status = 200;
    response.body = std::move(body);
    response.final_url = std::move(url);
    response.headers.emplace_back("Content-Type", "text/html");
    return response;
}

}  // namespace

// Reproduces the original bug: a JS-gated page (no-js class, noscript
// fallback, login form built by script through XHR) used to stay empty,
// making drivers report "login page requires JavaScript". The Flatworm web
// platform now executes such scripts.
TEST(ScriptPlatform, JsGatedPageBuildsItsLoginFormWithXhr) {
    Browser browser;
    auto network = std::make_unique<MemoryNetworkClient>();
    network->set_response("https://admin.example.test/", html(
        "<html class='no-js'><head><title>Sign in</title></head><body>"
        "<div id='mount'></div><noscript>enable JavaScript</noscript>"
        "<script>"
        "var x = new XMLHttpRequest();"
        "x.open('GET', '/api/form', false); x.send();"
        "document.getElementById('mount').innerHTML = x.responseText;"
        "document.documentElement.classList.remove('no-js');"
        "document.documentElement.classList.add('js');"
        "</script></body></html>", "https://admin.example.test/"));
    network->set_response("https://admin.example.test/api/form",
                         html("<form method='post' action='/login'>"
                              "<input name='username'>"
                              "<input type='password' name='password'>"
                              "</form>", ""));
    browser.set_network_client(std::move(network));

    auto session = browser.create_session();
    session->navigate("https://admin.example.test/");

    auto document = session->document();
    ASSERT_NE(document, nullptr);
    auto form = document->query_selector("#mount form[action='/login']");
    ASSERT_NE(form, nullptr) << "the page script did not mount the form";
    EXPECT_EQ(document->query_selector_all("input[type='password']").size(), 1u);
    // The engine also strips the no-js class and noscript fallbacks.
    auto html_element = document->query_selector("html");
    ASSERT_NE(html_element, nullptr);
    EXPECT_EQ(html_element->attribute("class"), "js");
    EXPECT_TRUE(document->query_selector_all("noscript").empty());
    // No ScriptException events were emitted.
    EXPECT_EQ(document->query_selector_all("form").size(), 1u);
}

TEST(ScriptPlatform, AsyncFetchAndTimersApplyBeforeNavigationReturns) {
    Browser browser;
    auto network = std::make_unique<MemoryNetworkClient>();
    network->set_response("https://app.test/", html(
        "<html><body><div id='d'>idle</div><script>"
        "fetch('/api/state').then(function (r) { return r.json(); })"
        "  .then(function (j) {"
        "    setTimeout(function () { document.getElementById('d').textContent = j.value; }, 0);"
        "  });"
        "</script></body></html>", "https://app.test/"));
    HttpResponse api;
    api.status = 200;
    api.body = "{\"value\":\"ready\"}";
    api.headers.emplace_back("Content-Type", "application/json");
    network->set_response("https://app.test/api/state", api);
    browser.set_network_client(std::move(network));

    auto session = browser.create_session();
    session->navigate("https://app.test/");
    auto div = session->document()->query_selector("#d");
    ASSERT_NE(div, nullptr);
    EXPECT_EQ(div->text(), "ready");
}

TEST(ScriptPlatform, DynamicallyAppendedExternalScriptsExecute) {
    Browser browser;
    auto network = std::make_unique<MemoryNetworkClient>();
    network->set_response("https://admin.example.test/", html(
        "<html><head></head><body><script>"
        "var clientlib = document.createElement('script');"
        "clientlib.onload = function () {"
        "  var challenge = document.createElement('script');"
        "  challenge.onload = function () {"
        "    document.body.setAttribute('data-token', window.as_token || '');"
        "  };"
        "  challenge.src = '/waf/challenge.js';"
        "  document.head.appendChild(challenge);"
        "};"
        "clientlib.src = 'https://xx.bstatic.com/libs/acc-clientlib/1/clientlib.js';"
        "document.head.appendChild(clientlib);"
        "</script></body></html>", "https://admin.example.test/"));
    HttpResponse clientlib;
    clientlib.status = 200;
    clientlib.final_url =
        "https://xx.bstatic.com/libs/acc-clientlib/1/clientlib.js";
    clientlib.body = "window.clientlib_loaded = true;";
    clientlib.headers.emplace_back("Content-Type", "application/javascript");
    network->set_response(
        "https://xx.bstatic.com/libs/acc-clientlib/1/clientlib.js",
        clientlib);
    HttpResponse challenge;
    challenge.status = 200;
    challenge.final_url = "https://admin.example.test/waf/challenge.js";
    challenge.body = "window.as_token = 'token-from-challenge';";
    challenge.headers.emplace_back("Content-Type", "application/javascript");
    network->set_response("https://admin.example.test/waf/challenge.js",
                          challenge);
    browser.set_network_client(std::move(network));

    auto session = browser.create_session();
    session->navigate("https://admin.example.test/");

    auto body = session->document()->query_selector("body");
    ASSERT_NE(body, nullptr);
    EXPECT_EQ(body->attribute("data-token"), "token-from-challenge");
    EXPECT_EQ(session->evaluate_js("clientlib_loaded && as_token"),
              "token-from-challenge");
}

TEST(ScriptPlatform, LocationAssignNavigatesAndCarriesCookies) {
    Browser browser;
    auto network = std::make_unique<MemoryNetworkClient>();
    network->set_response("https://site.test/", html(
        "<html><body><script>"
        "document.cookie = 'sid=abc123; Path=/';"
        "location.assign('/second');"
        "</script></body></html>", "https://site.test/"));
    network->set_response("https://site.test/second", html(
        "<html><head><title>Second</title></head>"
        "<body><p>arrived</p></body></html>", "https://site.test/second"));
    browser.set_network_client(std::move(network));

    auto session = browser.create_session();
    session->navigate("https://site.test/");

    EXPECT_EQ(session->current_url(), "https://site.test/second");
    EXPECT_EQ(session->document()->title(), "Second");
    // The cookie set from page script reached the jar and rode along on the
    // script-initiated navigation.
    bool carried = false;
    for (const auto& request :
         static_cast<MemoryNetworkClient&>(browser.network_client()).requests()) {
        for (const auto& [name, value] : request.headers) {
            if (name == "Cookie" && value.find("sid=abc123") != std::string::npos) {
                carried = true;
            }
        }
    }
    EXPECT_TRUE(carried);
}

TEST(ScriptPlatform, ScriptNavigationLoopIsBounded) {
    Browser browser;
    auto network = std::make_unique<MemoryNetworkClient>();
    // A hostile/buggy page that reloads itself forever.
    network->set_response("https://loop.test/", html(
        "<html><body><script>location.reload();</script></body></html>",
        "https://loop.test/"));
    browser.set_network_client(std::move(network));

    auto session = browser.create_session();
    session->navigate("https://loop.test/");
    EXPECT_EQ(static_cast<MemoryNetworkClient&>(browser.network_client())
                  .requests()
                  .size() <=
              static_cast<std::size_t>(browser.config().max_redirects) + 2,
              true);
}

TEST(ScriptPlatform, FormSubmitNavigatesWithEncodedQuery) {
    Browser browser;
    auto network = std::make_unique<MemoryNetworkClient>();
    network->set_response("https://shop.test/search?q=red%20shoes", html(
        "<html><head><title>Results</title></head><body></body></html>",
        "https://shop.test/search?q=red%20shoes"));
    network->set_response("https://shop.test/", html(
        "<html><body>"
        "<form id='f' action='/search' method='get'>"
        "<input name='q' value='red shoes'></form>"
        "<script>document.getElementById('f').submit();</script>"
        "</body></html>", "https://shop.test/"));
    browser.set_network_client(std::move(network));

    auto session = browser.create_session();
    session->navigate("https://shop.test/");
    EXPECT_EQ(session->document()->title(), "Results");
}

TEST(ScriptPlatform, WebStorageAndDomMutationsPersistToSessionStores) {
    Browser browser;
    auto network = std::make_unique<MemoryNetworkClient>();
    network->set_response("https://store.test/", html(
        "<html><body><script>"
        "localStorage.setItem('cart', '3');"
        "sessionStorage.setItem('lang', 'en');"
        "var el = document.createElement('span');"
        "el.id = 'built'; el.textContent = 'via script';"
        "document.body.appendChild(el);"
        "</script></body></html>", "https://store.test/"));
    browser.set_network_client(std::move(network));

    auto session = browser.create_session();
    session->navigate("https://store.test/");
    auto cart = session->local_storage().get("cart");
    ASSERT_TRUE(cart.has_value());
    EXPECT_EQ(*cart, "3");
    auto lang = session->session_storage().get("lang");
    ASSERT_TRUE(lang.has_value());
    EXPECT_EQ(*lang, "en");
    auto span = session->document()->get_element_by_id("built");
    ASSERT_NE(span, nullptr);
    EXPECT_EQ(span->text(), "via script");
}

TEST(ScriptPlatform, PageScriptErrorsSurfaceAsScriptExceptionEvents) {
    Browser browser;
    auto network = std::make_unique<MemoryNetworkClient>();
    network->set_response("https://err.test/", html(
        "<html><body><script>document.getElementById('missing').textContent = 'x';"
        "</script></body></html>", "https://err.test/"));
    browser.set_network_client(std::move(network));
    std::vector<std::string> exceptions;
    browser.events().subscribe(EventType::ScriptException,
                               [&](const Event& event) {
                                   exceptions.push_back(event.message);
                               });

    auto session = browser.create_session();
    EXPECT_NO_THROW(session->navigate("https://err.test/"));
    ASSERT_EQ(exceptions.size(), 1u);
    EXPECT_FALSE(exceptions[0].empty());
}

TEST(ScriptPlatform, DisabledJavascriptKeepsFallbackBehaviour) {
    prowsetk::BrowserConfig config;
    config.javascript = false;
    Browser browser(config);
    auto network = std::make_unique<MemoryNetworkClient>();
    network->set_response("https://off.test/", html(
        "<html class='no-js'><body><noscript>fallback</noscript><script>"
        "document.body.innerHTML = 'changed';</script></body></html>",
        "https://off.test/"));
    browser.set_network_client(std::move(network));

    auto session = browser.create_session();
    session->navigate("https://off.test/");
    // Without an engine the raw markup stays, and the unsupported API is
    // reported as an event rather than a failure.
    EXPECT_EQ(session->document()->query_selector_all("noscript").size(), 1u);
    EXPECT_NE(session->document()->query_selector("script"), nullptr);
}

// Synthetic Interaction Driver (README "Synthetic Interaction Driver & SPA
// Event Cascades"): Lua/C++ synthetic actions must run the full browser event
// sequences and drain microtasks so framework handlers reach the host.

namespace {

void install_cascade_recorder(prowsetk::Session& session, const std::string& selector) {
    session.evaluate_js(
        "window.__cascade = [];"
        "var __el = document.querySelector('" + selector + "');"
        "['pointerover','pointerenter','pointerdown','mousedown','focus',"
        " 'pointerup','mouseup','click'].forEach(function (t) {"
        "  __el.addEventListener(t, function () { window.__cascade.push(t); });"
        "});");
}

std::string cascade_log(prowsetk::Session& session) {
    return session.evaluate_js("window.__cascade.join('|')");
}

}  // namespace

TEST(ScriptPlatform, SyntheticClickDispatchesPointerCascadeInOrder) {
    Browser browser;
    auto network = std::make_unique<MemoryNetworkClient>();
    network->set_response("https://spa.test/", html(
        "<html><body><button id='go' type='button'>Go</button></body></html>",
        "https://spa.test/"));
    browser.set_network_client(std::move(network));

    auto session = browser.create_session();
    session->navigate("https://spa.test/");
    install_cascade_recorder(*session, "#go");

    auto button = session->document()->query_selector("#go");
    ASSERT_NE(button, nullptr);
    EXPECT_TRUE(session->click_element(button));
    EXPECT_EQ(cascade_log(*session),
              "pointerover|pointerenter|pointerdown|mousedown|focus|"
              "pointerup|mouseup|click");
}

TEST(ScriptPlatform, SyntheticClickSubmitControlFiresSubmitOnce) {
    Browser browser;
    auto network = std::make_unique<MemoryNetworkClient>();
    network->set_response("https://shop.test/", html(
        "<html><body>"
        "<form id='f' action='/search' method='get'>"
        "<input name='q' value='red shoes'>"
        "<button id='go' type='submit'>Search</button>"
        "</form>"
        "<script>"
        "window.__submits = 0;"
        "document.getElementById('f').addEventListener('submit', function () {"
        "  window.__submits += 1;"
        "});"
        "</script></body></html>", "https://shop.test/"));
    network->set_response("https://shop.test/search?q=red%20shoes", html(
        "<html><head><title>Results</title></head><body></body></html>",
        "https://shop.test/search?q=red%20shoes"));
    browser.set_network_client(std::move(network));

    auto session = browser.create_session();
    session->navigate("https://shop.test/");

    auto button = session->document()->query_selector("#go");
    ASSERT_NE(button, nullptr);
    EXPECT_TRUE(session->click_element(button));
    // Exactly one submit: the click default action fires it, with no duplicate
    // from the cascade itself.
    EXPECT_EQ(session->evaluate_js("window.__submits"), "1");
    EXPECT_EQ(session->document()->title(), "Results");
}

TEST(ScriptPlatform, SyntheticClickPreventDefaultSkipsSubmit) {
    Browser browser;
    auto network = std::make_unique<MemoryNetworkClient>();
    network->set_response("https://shop.test/", html(
        "<html><body>"
        "<form id='f' action='/search' method='get'>"
        "<button id='go' type='submit'>Search</button>"
        "</form>"
        "<script>"
        "window.__submits = 0;"
        "document.getElementById('go').addEventListener('click', function (e) {"
        "  e.preventDefault();"
        "});"
        "document.getElementById('f').addEventListener('submit', function () {"
        "  window.__submits += 1;"
        "});"
        "</script></body></html>", "https://shop.test/"));
    browser.set_network_client(std::move(network));

    auto session = browser.create_session();
    session->navigate("https://shop.test/");

    auto button = session->document()->query_selector("#go");
    ASSERT_NE(button, nullptr);
    EXPECT_FALSE(session->click_element(button));
    EXPECT_EQ(session->evaluate_js("window.__submits"), "0");
    EXPECT_EQ(session->current_url(), "https://shop.test/");
}

TEST(ScriptPlatform, SyntheticClickFailsFastOnNonInteractable) {
    Browser browser;
    auto network = std::make_unique<MemoryNetworkClient>();
    network->set_response("https://spa.test/", html(
        "<html><body>"
        "<button id='hidden' type='button' style='display: none'>H</button>"
        "<div id='wrap' hidden><button id='nested' type='button'>N</button></div>"
        "<button id='off' type='button' disabled>O</button>"
        "<button id='live' type='button'>L</button>"
        "<script>"
        "window.__clicks = 0;"
        "document.querySelectorAll('button').forEach(function (b) {"
        "  b.addEventListener('click', function () { window.__clicks += 1; });"
        "});"
        "</script></body></html>", "https://spa.test/"));
    browser.set_network_client(std::move(network));

    auto session = browser.create_session();
    session->navigate("https://spa.test/");

    for (const char* id : {"#hidden", "#nested", "#off"}) {
        auto element = session->document()->query_selector(id);
        ASSERT_NE(element, nullptr) << id;
        EXPECT_FALSE(session->click_element(element)) << id;
    }
    auto live = session->document()->query_selector("#live");
    ASSERT_NE(live, nullptr);
    EXPECT_TRUE(session->click_element(live));
    EXPECT_EQ(session->evaluate_js("window.__clicks"), "1");
}

TEST(ScriptPlatform, SyntheticTypeFiresKeyboardCascadeOncePerChar) {
    Browser browser;
    auto network = std::make_unique<MemoryNetworkClient>();
    network->set_response("https://spa.test/", html(
        "<html><body><input id='q' type='text' value=''>"
        "<script>"
        "window.__keys = [];"
        "var __input = document.getElementById('q');"
        "['keydown','keypress','input','keyup'].forEach(function (t) {"
        "  __input.addEventListener(t, function (e) {"
        "    window.__keys.push(t + ':' + (e.data || e.key || ''));"
        "  });"
        "});"
        "window.__changes = 0;"
        "__input.addEventListener('change', function () { window.__changes += 1; });"
        "</script></body></html>", "https://spa.test/"));
    browser.set_network_client(std::move(network));

    auto session = browser.create_session();
    session->navigate("https://spa.test/");

    auto input = session->document()->query_selector("#q");
    ASSERT_NE(input, nullptr);
    EXPECT_TRUE(session->type_element(input, "ab"));
    EXPECT_EQ(input->value(), "ab");
    // One canonical input event per character (no duplicate from the value
    // setter), followed by a single change on commit.
    EXPECT_EQ(session->evaluate_js("window.__keys.join('|')"),
              "keydown:a|keypress:a|input:a|keyup:a|"
              "keydown:b|keypress:b|input:b|keyup:b");
    EXPECT_EQ(session->evaluate_js("window.__changes"), "1");
}

TEST(ScriptPlatform, SyntheticTypeInvokesNativeSetterForControlledInputs) {
    Browser browser;
    auto network = std::make_unique<MemoryNetworkClient>();
    network->set_response("https://spa.test/", html(
        "<html><body><input id='q' type='text' value=''>"
        "<script>"
        "window.__nativeSets = [];"
        "var __input = document.getElementById('q');"
        "var __proto = Object.getPrototypeOf(__input);"
        "var __desc = Object.getOwnPropertyDescriptor(__proto, 'value');"
        "Object.defineProperty(__proto, 'value', {"
        "  get: __desc.get,"
        "  set: function (v) { window.__nativeSets.push(v); __desc.set.call(this, v); }"
        "});"
        "</script></body></html>", "https://spa.test/"));
    browser.set_network_client(std::move(network));

    auto session = browser.create_session();
    session->navigate("https://spa.test/");

    auto input = session->document()->query_selector("#q");
    ASSERT_NE(input, nullptr);
    EXPECT_TRUE(session->type_element(input, "hi"));
    // Each keystroke went through the native prototype setter, so framework
    // value trackers installed there observe every intermediate value.
    EXPECT_EQ(session->evaluate_js("window.__nativeSets.join('|')"), "h|hi");
    EXPECT_EQ(input->value(), "hi");
}

TEST(ScriptPlatform, SyntheticTypeRejectsNonInputsAndDisabled) {
    Browser browser;
    auto network = std::make_unique<MemoryNetworkClient>();
    network->set_response("https://spa.test/", html(
        "<html><body><div id='d'>text</div>"
        "<input id='off' type='text' disabled>"
        "<input id='on' type='text' value=''></body></html>",
        "https://spa.test/"));
    browser.set_network_client(std::move(network));

    auto session = browser.create_session();
    session->navigate("https://spa.test/");

    auto div = session->document()->query_selector("#d");
    ASSERT_NE(div, nullptr);
    EXPECT_FALSE(session->type_element(div, "x"));
    auto disabled = session->document()->query_selector("#off");
    ASSERT_NE(disabled, nullptr);
    EXPECT_FALSE(session->type_element(disabled, "x"));
    // Typing an empty string into a live input still focuses and commits.
    auto live = session->document()->query_selector("#on");
    ASSERT_NE(live, nullptr);
    EXPECT_TRUE(session->type_element(live, ""));
}

TEST(ScriptPlatform, SyntheticActionDrainsFrameworkFetchBeforeReturning) {
    Browser browser;
    auto network = std::make_unique<MemoryNetworkClient>();
    network->set_response("https://spa.test/", html(
        "<html><body><button id='go' type='button'>Go</button>"
        "<script>"
        "document.getElementById('go').addEventListener('click', function () {"
        "  fetch('/api/clicked', { method: 'POST', body: 'x=1' });"
        "});"
        "</script></body></html>", "https://spa.test/"));
    HttpResponse api;
    api.status = 200;
    api.body = "{}";
    api.headers.emplace_back("Content-Type", "application/json");
    network->set_response("https://spa.test/api/clicked", api);
    browser.set_network_client(std::move(network));

    auto session = browser.create_session();
    session->navigate("https://spa.test/");

    auto button = session->document()->query_selector("#go");
    ASSERT_NE(button, nullptr);
    EXPECT_TRUE(session->click_element(button));
    // The concluding __prowsetkFlush drained the framework fetch to the host
    // before the action returned.
    bool observed = false;
    for (const auto& request : session->page_script_requests()) {
        if (request.url == "https://spa.test/api/clicked" &&
            request.method == "POST") {
            observed = true;
        }
    }
    EXPECT_TRUE(observed);
}

TEST(ScriptPlatform, SyntheticActionsRejectInvalidAndClosedSessions) {
    Browser browser;
    auto network = std::make_unique<MemoryNetworkClient>();
    network->set_response("https://spa.test/", html(
        "<html><body><button id='go' type='button'>Go</button></body></html>",
        "https://spa.test/"));
    browser.set_network_client(std::move(network));

    auto session = browser.create_session();
    session->navigate("https://spa.test/");

    EXPECT_FALSE(session->click_element(nullptr));
    EXPECT_FALSE(session->type_element(nullptr, "x"));
    EXPECT_FALSE(session->click_element(std::make_shared<prowsetk::Element>()));
    auto button = session->document()->query_selector("#go");
    ASSERT_NE(button, nullptr);
    session->close();
    EXPECT_FALSE(session->click_element(button));
    EXPECT_FALSE(session->type_element(button, "x"));
}
