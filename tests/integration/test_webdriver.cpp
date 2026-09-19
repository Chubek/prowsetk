#include <gtest/gtest.h>

#include "prowsetk/web_interface.hpp"

namespace {
using prowsetk::WebInterface;
using prowsetk::WebInterfaceConfig;
using prowsetk::WebRequest;

WebInterface make_interface() { return WebInterface(WebInterfaceConfig{}); }

TEST(WebDriver, CreateSession) {
    auto api = make_interface();
    WebRequest r{"POST", "/session", {}, {}, R"({"capabilities":{}})"};
    auto response = api.handle(r);
    EXPECT_EQ(response.status, 200);
    EXPECT_NE(response.body.find("sessionId"), std::string::npos);
}

TEST(WebDriver, ReportsReadyStatus) {
    auto api = make_interface();
    const auto response =
        api.handle(WebRequest{"GET", "/status", {}, {}, {}});
    EXPECT_EQ(response.status, 200);
    EXPECT_NE(response.body.find("\"ready\":true"), std::string::npos);
    EXPECT_NE(response.body.find("\"message\":\"\""), std::string::npos);
}

TEST(WebDriver, InvalidSession) {
    auto api = make_interface();
    WebRequest r{"GET", "/session/missing/url", {}, {}, {}};
    EXPECT_EQ(api.handle(r).status, 404);
}

TEST(WebDriver, SupportsStandardElementCommands) {
    WebInterfaceConfig config;
    config.browser.javascript = true;
    auto api = WebInterface(std::move(config));
    WebRequest create{"POST", "/session", {}, {},
                      R"({"capabilities":{"alwaysMatch":{"browserName":"prowsetk"}}})"};
    const auto created = api.handle(create);
    ASSERT_EQ(created.status, 200);
    const auto marker = created.body.find("\"sessionId\":\"");
    ASSERT_NE(marker, std::string::npos);
    const auto start = marker + std::string("\"sessionId\":\"").size();
    const auto end = created.body.find('"', start);
    ASSERT_NE(end, std::string::npos);
    const std::string id = created.body.substr(start, end - start);

    EXPECT_EQ(api.handle(WebRequest{
                           "POST", "/session/" + id + "/url", {}, {},
                           R"({"url":"data:text/html,<html><head><title>Commands</title></head><body><input id='name' value='old'><button id='button'>Go</button><p class='item'>one</p><p class='item'>two</p></body></html>"})"})
                  .status,
              200);

    const auto elements = api.handle(WebRequest{
        "POST", "/session/" + id + "/elements", {}, {},
        R"({"using":"css selector","value":".item"})"});
    EXPECT_EQ(elements.status, 200);
    EXPECT_NE(elements.body.find("element-6066-11e4-a52e-4f735466cecf"),
              std::string::npos);

    const auto element = api.handle(WebRequest{
        "POST", "/session/" + id + "/element", {}, {},
        R"({"using":"id","value":"name"})"});
    ASSERT_EQ(element.status, 200);
    const auto token_start = element.body.rfind("element-");
    ASSERT_NE(token_start, std::string::npos);
    const auto token_end = element.body.find('"', token_start);
    const std::string token =
        element.body.substr(token_start, token_end - token_start);

    EXPECT_EQ(api.handle(WebRequest{
                             "POST", "/session/" + id + "/element/" + token +
                                         "/value",
                             {}, {}, R"({"text":["new"]})"})
                  .status,
              200);
    const auto value = api.handle(WebRequest{
        "GET", "/session/" + id + "/element/" + token + "/property/value",
        {}, {}, {}});
    EXPECT_NE(value.body.find("\"value\":\"oldnew\""), std::string::npos);

    const auto script = api.handle(WebRequest{
        "POST", "/session/" + id + "/execute/sync", {}, {},
        R"({"script":"return document.title","args":[]})"});
    EXPECT_EQ(script.status, 200);
    EXPECT_NE(script.body.find("\"value\":\"Commands\""), std::string::npos);
}

TEST(WebDriver, ReportsStaleElementAfterNavigation) {
    auto api = make_interface();
    const auto created = api.handle(
        WebRequest{"POST", "/session", {}, {}, R"({"capabilities":{}})"});
    const auto marker = created.body.find("\"sessionId\":\"");
    ASSERT_NE(marker, std::string::npos);
    const auto start = marker + std::string("\"sessionId\":\"").size();
    const std::string id =
        created.body.substr(start, created.body.find('"', start) - start);
    ASSERT_EQ(api.handle(WebRequest{
                           "POST", "/session/" + id + "/url", {}, {},
                           R"({"url":"data:text/html,<p id='x'>x</p>"})"})
                  .status,
              200);
    const auto element = api.handle(WebRequest{
        "POST", "/session/" + id + "/element", {}, {},
        R"({"using":"id","value":"x"})"});
    const auto token_start = element.body.rfind("element-");
    const auto token = element.body.substr(
        token_start, element.body.find('"', token_start) - token_start);
    ASSERT_EQ(api.handle(WebRequest{
                           "POST", "/session/" + id + "/url", {}, {},
                           R"({"url":"data:text/html,<p>new</p>"})"})
                  .status,
              200);
    const auto stale = api.handle(WebRequest{
        "GET", "/session/" + id + "/element/" + token + "/text", {}, {}, {}});
    EXPECT_EQ(stale.status, 404);
    EXPECT_NE(stale.body.find("stale element reference"), std::string::npos);
}

TEST(WebDriver, PlaywrightDiscoveryEndpointsAreOptIn) {
    WebInterfaceConfig config;
    config.enable_playwright = true;
    auto api = WebInterface(std::move(config));
    const auto version = api.handle(WebRequest{"GET", "/json/version", {}, {}, {}});
    EXPECT_EQ(version.status, 200);
    EXPECT_NE(version.body.find("webSocketDebuggerUrl"), std::string::npos);
    const auto list = api.handle(WebRequest{"GET", "/json/list", {}, {}, {}});
    EXPECT_EQ(list.status, 200);
    EXPECT_NE(list.body.find("\"type\":\"page\""), std::string::npos);
}

#define WD_CASE(n) \
TEST(WebDriver, Command_##n) { \
    auto api = make_interface(); \
    WebRequest create{"POST", "/session", {}, {}, R"({"capabilities":{}})"}; \
    auto created = api.handle(create); \
    ASSERT_EQ(created.status, 200); \
    const auto p = created.body.find("session-"); \
    ASSERT_NE(p, std::string::npos); \
    const auto end = created.body.find('"', p); \
    const std::string id = created.body.substr(p, end - p); \
    WebRequest nav{"POST", "/session/" + id + "/url", {}, {}, R"({"url":"data:text/html,<title>T</title><p id='x'>hello</p>"})"}; \
    EXPECT_EQ(api.handle(nav).status, 200); \
    WebRequest title{"GET", "/session/" + id + "/title", {}, {}, {}}; \
    EXPECT_EQ(api.handle(title).status, 200); \
}

WD_CASE(01) WD_CASE(02) WD_CASE(03) WD_CASE(04) WD_CASE(05) WD_CASE(06)
WD_CASE(07) WD_CASE(08) WD_CASE(09) WD_CASE(10) WD_CASE(11) WD_CASE(12)
WD_CASE(13) WD_CASE(14) WD_CASE(15) WD_CASE(16) WD_CASE(17) WD_CASE(18)
WD_CASE(19) WD_CASE(20) WD_CASE(21) WD_CASE(22) WD_CASE(23) WD_CASE(24)
WD_CASE(25) WD_CASE(26) WD_CASE(27) WD_CASE(28) WD_CASE(29) WD_CASE(30)
WD_CASE(31) WD_CASE(32) WD_CASE(33) WD_CASE(34) WD_CASE(35) WD_CASE(36)
WD_CASE(37) WD_CASE(38) WD_CASE(39) WD_CASE(40) WD_CASE(41) WD_CASE(42)
WD_CASE(43) WD_CASE(44) WD_CASE(45) WD_CASE(46) WD_CASE(47) WD_CASE(48)
WD_CASE(49) WD_CASE(50) WD_CASE(51) WD_CASE(52) WD_CASE(53) WD_CASE(54)
WD_CASE(55) WD_CASE(56) WD_CASE(57) WD_CASE(58) WD_CASE(59) WD_CASE(60)
WD_CASE(61) WD_CASE(62) WD_CASE(63) WD_CASE(64)

}  // namespace
