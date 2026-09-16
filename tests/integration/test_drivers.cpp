#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "prowsetk/lua_runtime.hpp"

using prowsetk::LuaArgument;
using prowsetk::LuaRuntime;

namespace {

std::string read_file(const std::string& path) {
    std::ifstream stream(path);
    if (!stream) {
        return {};
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

std::string driver_path(const std::string& name) {
    return std::string(PROWSETK_SOURCE_DIR) + "/drivers/" + name;
}

constexpr const char* kCrawlHtml =
    "<html><head><title>Fixture</title></head><body>"
    "<a href=\"/a\">A</a><a href=\"/b\">B</a></body></html>";

constexpr const char* kLoginHtml =
    "<html><body><form action=\"/auth\" method=\"post\">"
    "<input name=\"email\"><input name=\"password\" type=\"password\">"
    "</form></body></html>";

}  // namespace

// Each shipped driver loads cleanly and defines the `main` entrypoint.
TEST(Drivers, LoadAndExposeEntrypoint) {
    if (!LuaRuntime::available()) {
        GTEST_SKIP() << "ProwseTk was built without Lua support";
    }
    for (const char* name : {"crawl_site.lua", "login.lua"}) {
        LuaRuntime lua;
        ASSERT_TRUE(lua.run_file(driver_path(name)).ok)
            << name << ": " << lua.last_error();
        const auto check =
            lua.run("assert(type(main) == 'function', "
                    "'driver did not define main')",
                    "driver_entrypoint");
        EXPECT_TRUE(check.ok) << name << ": " << lua.last_error();
    }
}

TEST(Drivers, CrawlSiteWritesPageMetadata) {
    if (!LuaRuntime::available()) {
        GTEST_SKIP() << "ProwseTk was built without Lua support";
    }
    const std::string output =
        std::string(TEST_BINARY_DIR) + "/driver_crawl_pages.jsonl";
    std::remove(output.c_str());

    LuaRuntime lua;
    ASSERT_TRUE(lua.run_file(driver_path("crawl_site.lua")).ok)
        << lua.last_error();

    const std::vector<LuaArgument> args = {
        {"url", "string", "https://example.com/"},
        {"depth", "integer", "0"},
        {"output", "path", output},
        {"html", "string", kCrawlHtml},
    };
    std::string exit_code;
    ASSERT_TRUE(lua.call_function("main", args, &exit_code).ok)
        << lua.last_error();
    EXPECT_EQ(exit_code, "0");

    const std::string content = read_file(output);
    ASSERT_FALSE(content.empty());
    EXPECT_NE(content.find("\"url\":\"https://example.com/\""),
              std::string::npos);
    EXPECT_NE(content.find("\"title\":\"Fixture\""), std::string::npos);
    EXPECT_NE(content.find("\"links\":[\"/a\",\"/b\"]"), std::string::npos);
    EXPECT_NE(content.find("\"link_count\":2"), std::string::npos);
    EXPECT_NE(content.find("\"offline\":true"), std::string::npos);
}

TEST(Drivers, CrawlSiteRejectsMissingUrl) {
    if (!LuaRuntime::available()) {
        GTEST_SKIP() << "ProwseTk was built without Lua support";
    }
    LuaRuntime lua;
    ASSERT_TRUE(lua.run_file(driver_path("crawl_site.lua")).ok)
        << lua.last_error();
    std::string exit_code;
    const auto result = lua.call_function("main", {{"html", "string", "<i>"}},
                                          &exit_code);
    EXPECT_FALSE(result.ok);
    EXPECT_NE(lua.last_error().find("url"), std::string::npos);
}

TEST(Drivers, LoginFillsFormAndRedactsCredentials) {
    if (!LuaRuntime::available()) {
        GTEST_SKIP() << "ProwseTk was built without Lua support";
    }
    const std::string output =
        std::string(TEST_BINARY_DIR) + "/driver_login.json";
    std::remove(output.c_str());

    LuaRuntime lua;
    ASSERT_TRUE(lua.run_file(driver_path("login.lua")).ok) << lua.last_error();

    const std::vector<LuaArgument> args = {
        {"url", "string", "https://example.com/login"},
        {"username", "string", "alice"},
        {"password", "string", "supersecret"},
        {"output", "path", output},
        {"html", "string", kLoginHtml},
    };
    std::string exit_code;
    ASSERT_TRUE(lua.call_function("main", args, &exit_code).ok)
        << lua.last_error();
    EXPECT_EQ(exit_code, "0");

    const std::string content = read_file(output);
    ASSERT_FALSE(content.empty());
    EXPECT_NE(content.find("\"submitted\":false"), std::string::npos);
    EXPECT_NE(content.find("\"credentials_redacted\":true"),
              std::string::npos);
    EXPECT_NE(content.find("\"username_field\":\"email\""),
              std::string::npos);
    EXPECT_NE(content.find("\"password_field\":\"password\""),
              std::string::npos);
    EXPECT_NE(content.find("\"method\":\"POST\""), std::string::npos);
    EXPECT_NE(content.find("\"action\":\"https://example.com/auth\""),
              std::string::npos);
    // Secret credentials must never reach the output file.
    EXPECT_EQ(content.find("supersecret"), std::string::npos);
    EXPECT_EQ(content.find("alice"), std::string::npos);
}

TEST(Drivers, LoginRejectsMissingPasswordField) {
    if (!LuaRuntime::available()) {
        GTEST_SKIP() << "ProwseTk was built without Lua support";
    }
    LuaRuntime lua;
    ASSERT_TRUE(lua.run_file(driver_path("login.lua")).ok) << lua.last_error();

    const std::vector<LuaArgument> args = {
        {"url", "string", "https://example.com/"},
        {"username", "string", "alice"},
        {"password", "string", "supersecret"},
        {"html", "string", "<html><body><form action=\"/a\">"
                           "<input name=\"email\"></form></body></html>"},
    };
    std::string exit_code;
    const auto result = lua.call_function("main", args, &exit_code);
    EXPECT_FALSE(result.ok);
    EXPECT_NE(lua.last_error().find("password"), std::string::npos);
}