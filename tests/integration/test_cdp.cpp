#include <gtest/gtest.h>

#include <sstream>
#include <string>

#include "playwright.hpp"
#include "prowsetk/web_interface.hpp"

using prowsetk::WebInterface;
using prowsetk::WebInterfaceConfig;
using prowsetk::WebRequest;
using prowsetk::WebResponse;

namespace {

WebInterface make_cdp_interface() {
    WebInterfaceConfig config;
    config.browser.javascript = true;
    config.enable_playwright = true;
    config.web_root.clear();
    return WebInterface(config);
}

std::string extract_session_id(const std::string& body) {
    const auto pos = body.find("sessionId");
    if (pos == std::string::npos) return "";
    const auto quote1 = body.find('"', pos + 10);
    if (quote1 == std::string::npos) return "";
    const auto quote2 = body.find('"', quote1 + 1);
    if (quote2 == std::string::npos) return "";
    return body.substr(quote1 + 1, quote2 - quote1 - 1);
}

TEST(CDP, VersionEndpoint) {
    auto api = make_cdp_interface();
    WebRequest r{"GET", "/json/version", {}, {}, {}};
    auto response = api.handle(r);
    EXPECT_EQ(response.status, 200);
    EXPECT_NE(response.body.find("ProwseTk"), std::string::npos);
}

TEST(CDP, TargetListEndpoint) {
    auto api = make_cdp_interface();
    WebRequest r{"GET", "/json/list", {}, {}, {}};
    auto response = api.handle(r);
    EXPECT_EQ(response.status, 200);
    // /json/list returns a JSON array of target info objects
    EXPECT_NE(response.body.find("page"), std::string::npos);
    EXPECT_NE(response.body.find("description"), std::string::npos);
    EXPECT_NE(response.body.find("devtoolsFrontendUrl"), std::string::npos);
}

TEST(CDP, UnknownEndpointReturns404) {
    auto api = make_cdp_interface();
    WebRequest r{"GET", "/json/unknown", {}, {}, {}};
    auto response = api.handle(r);
    EXPECT_EQ(response.status, 404);
}

TEST(CDP, NonGETReturns405) {
    auto api = make_cdp_interface();
    WebRequest r{"POST", "/json/version", {}, {}, {}};
    auto response = api.handle(r);
    EXPECT_EQ(response.status, 405);
}

TEST(CDP, WebInterfaceSessionLifecycle) {
    auto api = make_cdp_interface();
    // Create a session via WebDriver protocol
    WebRequest create{"POST", "/session", {}, {}, R"({"capabilities":{}})"};
    auto created = api.handle(create);
    EXPECT_EQ(created.status, 200);
    EXPECT_NE(created.body.find("sessionId"), std::string::npos);

    // Extract session ID from response
    const std::string session_id = extract_session_id(created.body);
    EXPECT_FALSE(session_id.empty());

    // Navigate via WebDriver
    WebRequest nav{"POST", "/session/" + session_id + "/url", {}, {}, R"({"url":"data:text/html,<title>Test</title>"})"};
    auto navged = api.handle(nav);
    EXPECT_EQ(navged.status, 200);

    // Get title
    WebRequest title{"GET", "/session/" + session_id + "/title", {}, {}, {}};
    auto t = api.handle(title);
    EXPECT_EQ(t.status, 200);
    EXPECT_NE(t.body.find("Test"), std::string::npos);

    // Verify the CDP endpoint also works
    WebRequest cdp{"GET", "/json/version", {}, {}, {}};
    auto cdp_resp = api.handle(cdp);
    EXPECT_EQ(cdp_resp.status, 200);
}

TEST(CDP, WebDriverStatusEndpoint) {
    auto api = make_cdp_interface();
    WebRequest r{"GET", "/session", {}, {}, {}};
    auto response = api.handle(r);
    EXPECT_EQ(response.status, 200);
}

TEST(CDP, WebDriverDeleteSession) {
    auto api = make_cdp_interface();
    WebRequest create{"POST", "/session", {}, {}, R"({"capabilities":{}})"};
    auto created = api.handle(create);
    EXPECT_EQ(created.status, 200);

    // Extract session ID from response
    const std::string session_id = extract_session_id(created.body);
    EXPECT_FALSE(session_id.empty());

    // Delete the session
    WebRequest del{"DELETE", "/session/" + session_id, {}, {}, {}};
    auto deleted = api.handle(del);
    EXPECT_EQ(deleted.status, 200);
}

TEST(CDP, WebDriverGetSource) {
    auto api = make_cdp_interface();
    WebRequest create{"POST", "/session", {}, {}, R"({"capabilities":{}})"};
    auto created = api.handle(create);
    EXPECT_EQ(created.status, 200);
    const std::string session_id = extract_session_id(created.body);

    // Navigate and check source
    WebRequest nav{"POST", "/session/" + session_id + "/url", {}, {}, R"({"url":"data:text/html,<p id='x'>hello</p>"})"};
    auto navged = api.handle(nav);
    EXPECT_EQ(navged.status, 200);

    WebRequest src{"GET", "/session/" + session_id + "/source", {}, {}, {}};
    auto s = api.handle(src);
    EXPECT_EQ(s.status, 200);
    EXPECT_NE(s.body.find("hello"), std::string::npos);
}

TEST(CDP, WebDriverElements) {
    auto api = make_cdp_interface();
    WebRequest create{"POST", "/session", {}, {}, R"({"capabilities":{}})"};
    auto created = api.handle(create);
    EXPECT_EQ(created.status, 200);
    const std::string session_id = extract_session_id(created.body);

    WebRequest nav{"POST", "/session/" + session_id + "/url", {}, {}, R"({"url":"data:text/html,<div class='box'>a</div><div class='box'>b</div>"})"};
    api.handle(nav);

    // Find elements by CSS selector
    WebRequest find{"POST", "/session/" + session_id + "/elements", {}, {}, R"({"using":"css selector","value":".box"})"};
    auto found = api.handle(find);
    EXPECT_EQ(found.status, 200);
    EXPECT_NE(found.body.find("element-6066"), std::string::npos);
}

}  // namespace
