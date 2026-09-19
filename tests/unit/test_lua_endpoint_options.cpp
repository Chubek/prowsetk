#include <gtest/gtest.h>
#include "prowsetk/lua_runtime.hpp"

TEST(LuaEndpointOptions, ComprehensiveDiscoveryIsExplicit) {
    if (!prowsetk::LuaRuntime::available()) GTEST_SKIP();
    prowsetk::LuaRuntime lua;
    const auto result = lua.run(R"lua(
        local browser = require('lprowse').browser.new({javascript=false})
        local session = browser:create_session()
        session:load_html('<a href="/reservations">Reservations</a>', 'https://fixture.example/')
        local endpoints = require('lprowsext').endpoints
        assert(endpoints.extract(session:document(), {minimum_confidence=0}):endpoint_count() == 0)
        local all = endpoints.extract(session:document(), {scrape_all_paths=true, minimum_confidence=0})
        assert(all:endpoint_count() == 1)
        assert(all:openapi_yaml():find('/reservations', 1, true))
        session:close()
    )lua");
    EXPECT_TRUE(result.ok) << result.error;
}
