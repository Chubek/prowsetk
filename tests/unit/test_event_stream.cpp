#include <gtest/gtest.h>
#include "core/event_stream.hpp"
#include "prowsetk/error.hpp"

using namespace prowsetk;

TEST(EventStream, PreservesOrderAndEarlyStop) {
    auto document = parse_html("<p>Hello <b>world</b></p>");
    EventStreamReader reader(*document);
    const auto expected = emit_prowse_events(*document);
    std::size_t delivered = 0;
    EXPECT_TRUE(reader.replay([&](const ProwseEvent& event) {
        EXPECT_EQ(event.kind, expected[delivered].kind);
        EXPECT_EQ(event.xpath, expected[delivered].xpath);
        ++delivered;
        return EventStreamAction::Continue;
    }));
    EXPECT_EQ(delivered, expected.size());
    delivered = 0;
    EXPECT_TRUE(reader.replay([&](const ProwseEvent&) {
        ++delivered; return EventStreamAction::Stop;
    }));
    EXPECT_EQ(delivered, 1u);
    EXPECT_FALSE(reader.replay([](const ProwseEvent&) { return EventStreamAction::Continue; }, {0}));
}

TEST(EventStream, OwnsSnapshotAndPropagatesFailures) {
    auto document = parse_html("<p>before</p>");
    EventStreamReader reader(*document);
    document->query_selector("p")->set_text("after");
    EXPECT_EQ(reader.events()[0].subtree_text, "before");
    EXPECT_THROW(reader.replay({}), Error);
    EXPECT_THROW(reader.replay([](const ProwseEvent&) { return EventStreamAction::Continue; },
                              {max_event_stream_events + 1}), Error);
    EXPECT_THROW(reader.replay([](const ProwseEvent&) -> EventStreamAction {
        throw Error(ErrorCode::Timeout, "callback timeout");
    }), Error);
}

TEST(EventStream, EnforcesInputBounds) {
    EXPECT_THROW(EventStreamReader(ProwseEventStream(max_event_stream_events + 1)), Error);
    ProwseEvent event;
    event.value.assign(max_event_stream_bytes + 1, 'x');
    EXPECT_THROW(EventStreamReader(ProwseEventStream{std::move(event)}), Error);
    event = {};
    event.attributes.resize(max_event_stream_events + 1);
    EXPECT_THROW(EventStreamReader(ProwseEventStream{std::move(event)}), Error);
}
