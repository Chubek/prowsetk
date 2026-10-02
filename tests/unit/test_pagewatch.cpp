#include <gtest/gtest.h>
#include "pagewatch.hpp"
#include "lua_sources.hpp"
#include <prowsetk/error.hpp>
#include <filesystem>
#include <sys/stat.h>

namespace pa = prowsetk::automation;
TEST(Pagewatch, ConfigurationBoundsAndQueryLegality) {
    const auto config = prowsetk::pagewatch::parse_watch_config("[watcher]\nurl='https://example.test/'\nquery='select text from <h1>'\ninterval_ms=20\n");
    EXPECT_EQ(config.interval_ms, 20);
    EXPECT_THROW(prowsetk::pagewatch::parse_watch_config("[watcher]\nurl='https://example.test/'\ninterval_ms=0"), std::exception);
    EXPECT_THROW(prowsetk::pagewatch::parse_watch_config("[watcher]\nurl='https://example.test/'\nquery='select nope from <h1>'"), std::exception);
    EXPECT_THROW(prowsetk::pagewatch::parse_watch_config("[watcher]\nurl='file:///etc/passwd'"), std::exception);
    EXPECT_THROW(prowsetk::pagewatch::parse_watch_config("[watcher]\nurl='https://example.test/'\nmisspelled=true"), std::exception);
}
TEST(Pagewatch, FramingIsIncrementalAndBounded) {
    const std::string payload("line\n\0bytes", 11);
    auto buffer = pa::frame({"deploy", "name", payload});
    auto prefix = buffer.substr(0, 3);
    EXPECT_FALSE(pa::take_frame(prefix));
    prefix += buffer.substr(3);
    const auto decoded = pa::take_frame(prefix);
    ASSERT_TRUE(decoded); EXPECT_EQ((*decoded)[0], "deploy"); EXPECT_TRUE(prefix.empty());
    EXPECT_EQ((*decoded)[2], payload);
    std::string large("\x7f\xff\xff\xff", 4);
    EXPECT_THROW(pa::take_frame(large), std::exception);
    EXPECT_THROW(pa::frame({std::string(pa::message_limit + 1, 'a')}), std::exception);
    EXPECT_FALSE(pa::valid_name("../escape")); EXPECT_FALSE(pa::valid_name("x;y")); EXPECT_TRUE(pa::valid_name("booking.admin-1"));
}
TEST(Pagewatch, TransportPolicyGuardsRedirectAndScriptDestinations) {
    auto memory = std::make_unique<prowsetk::MemoryNetworkClient>();
    auto* observed = memory.get();
    prowsetk::HttpResponse response; response.status = 200;
    memory->set_response("https://example.test/", response);
    pa::RestrictedNetwork network({"https://example.test"}, 1, std::move(memory));
    auto request = [](const char* url) { prowsetk::HttpRequest value; value.url = url; return value; };
    EXPECT_THROW(network.send(request("https://elsewhere.test/")), std::exception);
    EXPECT_THROW(network.send(request("https://user:pass@example.test/")), std::exception);
    EXPECT_TRUE(network.send(request("https://example.test/")).ok());
    EXPECT_THROW(network.send(request("https://example.test/")), prowsetk::Error);
    EXPECT_EQ(observed->requests().size(), 1u);
}
TEST(Pagewatch, ManagedLuaWatcherDetectsOnlyProjectedChanges) {
    prowsetk::BrowserConfig config; config.javascript = false;
    prowsetk::Browser browser(config);
    auto session = browser.create_session(); session->load_html("<h1>A</h1><p>outside</p>");
    prowsetk::LuaRuntime lua; lua.bind_browser(&browser); lua.bind_session(session);
    pa::load_module(lua, "lpgwatch", pa::lpgwatch_source);
    ASSERT_TRUE(lua.run(R"(
        w = require('lpgwatch').watch {session=session, query='select text from <h1>'}
        local first, changed = w:sample(); assert(changed and first == '[{"text":"A"}]')
        session:document():query_selector('p'):set_text('unselected mutation')
        local same, changed = w:sample(); assert(not changed and same == first)
        session:document():query_selector('h1'):set_text('B')
        local different, changed = w:sample(); assert(changed and different == '[{"text":"B"}]')
        assert(not pcall(require('lpgwatch').watch, {session=session, query='<p>'}))
    )").ok) << lua.last_error();
}
TEST(Pagewatch, OwnerOnlyAtomicFilesAndSymlinkRejection) {
    const auto root = std::filesystem::path(TEST_BINARY_DIR) / "pagewatch-private";
    std::filesystem::remove_all(root); pa::private_directory(root);
    pa::write_file(root / "tab.json", "one"); pa::write_file(root / "tab.json", "two");
    struct stat status{}; ASSERT_EQ(::stat((root / "tab.json").c_str(), &status), 0);
    EXPECT_EQ(status.st_mode & 0777, 0600); EXPECT_EQ(pa::read_file(root / "tab.json"), "two");
    std::filesystem::create_directory_symlink(root, root.parent_path() / "pagewatch-link");
    EXPECT_THROW(pa::private_directory(root.parent_path() / "pagewatch-link"), std::exception);
    std::filesystem::remove(root.parent_path() / "pagewatch-link"); std::filesystem::remove_all(root);
}
