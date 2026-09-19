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

TEST(WebDriver, InvalidSession) {
    auto api = make_interface();
    WebRequest r{"GET", "/session/missing/url", {}, {}, {}};
    EXPECT_EQ(api.handle(r).status, 404);
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
