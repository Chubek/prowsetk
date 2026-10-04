#include <gtest/gtest.h>
#include <cmath>
#include <limits>
#include <prowsetk/browser.hpp>
#include <prowsetk/error.hpp>
#include <prowsetk/flatworm_host.hpp>

using namespace prowsetk;

TEST(Viewport, HostValidatesAtomicallyAndPersistsAcrossDocuments) {
    auto host = make_detached_script_host(parse_html("<p>One</p>"));
    EXPECT_EQ(host->viewport_info().width, 1280);
    host->set_viewport({640, 480, 2});
    for (auto bad : {ViewportInfo{0, 480, 1}, ViewportInfo{16400, 480, 1},
                     ViewportInfo{640, -1, 1}, ViewportInfo{640, 480, 0},
                     ViewportInfo{640, 480, 9},
                     ViewportInfo{640, 480, std::numeric_limits<double>::quiet_NaN()}}) {
        EXPECT_THROW(host->set_viewport(bad), Error);
        EXPECT_EQ(host->viewport_info().width, 640);
        EXPECT_EQ(host->viewport_info().device_pixel_ratio, 2);
    }
    host->install(parse_html("<p>Two</p>"), "https://example.test/");
    EXPECT_EQ(host->viewport_info().height, 480);
}

TEST(Viewport, SessionWorksWithoutJavaScriptAndRejectsClosedCalls) {
    BrowserConfig config;
    config.javascript = false;
    Browser browser(config);
    auto session = browser.create_session();
    session->set_viewport({500, 700, 1});
    session->load_html("<p>Static</p>");
    session->pump_events();
    EXPECT_EQ(session->viewport().width, 500);
    session->close();
    EXPECT_THROW(session->set_viewport({600, 800, 1}), Error);
    EXPECT_THROW(session->pump_events(), Error);
}

TEST(Viewport, LiveMetadataMediaQueriesAndResizeEventsAgree) {
    auto host = make_detached_script_host(parse_html("<html><body></body></html>"));
    auto runtime = make_javascript_runtime(host.get());
    if (runtime->name() == "null") GTEST_SKIP();
    ASSERT_TRUE(runtime->evaluate(R"JS(
      var mq = matchMedia('screen and (max-width: 700px)');
      var changes = [], resized = 0, visual = 0;
      mq.addListener(e => changes.push(e.matches));
      window.onresize = () => resized++;
      visualViewport.onresize = () => visual++;
    )JS").ok);
    host->set_viewport({600, 900, 2});
    ASSERT_TRUE(runtime->evaluate("__prowsetkViewportChanged()").ok);
    auto value = [&](const char* js) { return runtime->evaluate(js).value; };
    EXPECT_EQ(value("[innerWidth,innerHeight,outerWidth,screen.availHeight,visualViewport.width,devicePixelRatio].join(',')"), "600,900,600,900,600,2");
    EXPECT_EQ(value("changes.join(',') + ':' + resized + ':' + visual"), "true:1:1");
    EXPECT_EQ(value("matchMedia('(orientation: portrait) and (min-resolution: 2dppx)').matches"), "true");
    EXPECT_EQ(value("matchMedia('print, (min-width: 500px)').matches"), "true");
    EXPECT_EQ(value("matchMedia('not print').matches"), "true");
    EXPECT_EQ(value("matchMedia('not (unsupported: value)').matches"), "false");
    EXPECT_EQ(value("matchMedia('(min-width: 20em)').matches"), "false");
    EXPECT_EQ(value("matchMedia('(prefers-color-scheme: dark)').matches"), "false");
    EXPECT_FALSE(runtime->evaluate("matchMedia('x'.repeat(4097))").ok);
    host->set_viewport({900, 600, 1});
    ASSERT_TRUE(runtime->evaluate("__prowsetkViewportChanged()").ok);
    EXPECT_EQ(value("changes.join(',')"), "true,false");
}

TEST(Viewport, MediaListsHaveAFiniteLiveAllocationLimit) {
    auto host = make_detached_script_host(parse_html("<html></html>"));
    auto runtime = make_javascript_runtime(host.get());
    if (runtime->name() == "null") GTEST_SKIP();
    ASSERT_TRUE(runtime->evaluate("var lists = []; for(var i=0;i<256;i++) lists.push(matchMedia('screen'));").ok);
    EXPECT_FALSE(runtime->evaluate("lists.push(matchMedia('screen'))").ok);
    EXPECT_TRUE(runtime->capabilities().has("viewport"));
    EXPECT_EQ(runtime->capabilities().classification("matchMedia"), ImplementationClass::PartiallyImplemented);
}
