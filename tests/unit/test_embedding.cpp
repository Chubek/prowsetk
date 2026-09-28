#include <gtest/gtest.h>

#include <string>

#include "prowsetk/embedding.hpp"
#include "prowsetk/error.hpp"

TEST(Embedding, LoadsAndStreamsCanonicalEvents) {
    prowsetk::BrowserConfig config;
    config.javascript = false;
    prowsetk::EmbeddedBrowser browser(config);
    const auto& document = browser.load_html(
        "<html><body><p class='intro'>hello</p></body></html>",
        "https://example.test/");
    EXPECT_TRUE(document.valid());

    std::size_t starts = 0;
    const auto result = browser.stream_events([&](const prowsetk::ProwseEvent& event) {
        if (event.kind == "start") {
            ++starts;
        }
        return prowsetk::EventStreamControl::Continue;
    });
    EXPECT_TRUE(result.complete);
    EXPECT_EQ(result.delivered, browser.events().size());
    EXPECT_GE(starts, 3U);
}

TEST(Embedding, AllowsAVisitorToStopWithoutChangingTheDocument) {
    prowsetk::EmbeddedBrowser browser;
    browser.load_html("<p>stop</p>");
    const auto result = browser.stream_events([](const prowsetk::ProwseEvent&) {
        return prowsetk::EventStreamControl::Stop;
    });
    EXPECT_FALSE(result.complete);
    EXPECT_EQ(result.delivered, 1U);
    EXPECT_TRUE(browser.document().valid());
}

TEST(Embedding, RejectsMissingDocumentAndEmptyVisitors) {
    prowsetk::EmbeddedBrowser browser;
    try {
        (void)browser.document();
        FAIL() << "expected no-document error";
    } catch (const prowsetk::Error& error) {
        EXPECT_EQ(error.code(), prowsetk::ErrorCode::NotFound);
    }

    browser.load_html("<p>ready</p>");
    try {
        (void)browser.stream_events({});
        FAIL() << "expected empty-visitor error";
    } catch (const prowsetk::Error& error) {
        EXPECT_EQ(error.code(), prowsetk::ErrorCode::InvalidArgument);
    }
}
