#include <gtest/gtest.h>
#include "prowsetk/embedding.hpp"
#include "prowsetk/gfx_backend.hpp"

TEST(GfxPipeline, EmbeddedStreamFeedsIndependentBackendAndRefreshesAfterMutation) {
    prowsetk::BrowserConfig config;
    config.javascript = false;
    prowsetk::EmbeddedBrowser browser(config);
    browser.load_html("<p>Before</p><textarea>PRIVATE</textarea>");
    prowsetk::GfxBackend backend("headless");
    const auto render = [&]() {
        prowsetk::ProwseEventStream events;
        const auto result = browser.stream_events([&](const prowsetk::ProwseEvent& event) {
            events.push_back(event); return prowsetk::EventStreamControl::Continue;
        });
        EXPECT_TRUE(result.complete);
        EXPECT_EQ(result.delivered, events.size());
        const auto bytes = prowsetk::emit_gfx_ir(events);
        backend.submit(bytes); backend.run();
        return prowsetk::decode_gfx_frame(bytes);
    };
    EXPECT_EQ(render().text[0].text, "Before");
    browser.document().query_selector("p")->set_text("After");
    const auto refreshed = render();
    ASSERT_EQ(refreshed.text.size(), 1u);
    EXPECT_EQ(refreshed.text[0].text, "After");
}
