#include <gtest/gtest.h>

#include "prowsetk/event.hpp"

using prowsetk::Event;
using prowsetk::EventDispatcher;
using prowsetk::EventType;

TEST(Events, DeliversToMatchingHandlers) {
    EventDispatcher dispatcher;
    int navigation_count = 0;
    int console_count = 0;
    dispatcher.subscribe(EventType::BeforeNavigation,
                         [&](Event&) { ++navigation_count; });
    dispatcher.subscribe(EventType::Console,
                         [&](Event&) { ++console_count; });

    Event navigation;
    navigation.type = EventType::BeforeNavigation;
    dispatcher.emit(navigation);

    EXPECT_EQ(navigation_count, 1);
    EXPECT_EQ(console_count, 0);
}

TEST(Events, GlobalHandlersReceiveEverything) {
    EventDispatcher dispatcher;
    int count = 0;
    dispatcher.subscribe_all([&](Event&) { ++count; });

    Event first;
    first.type = EventType::Console;
    Event second;
    second.type = EventType::AfterNavigation;
    dispatcher.emit(first);
    dispatcher.emit(second);

    EXPECT_EQ(count, 2);
}

TEST(Events, UnsubscribeStopsDelivery) {
    EventDispatcher dispatcher;
    int count = 0;
    const auto id =
        dispatcher.subscribe(EventType::Console, [&](Event&) { ++count; });
    Event event;
    event.type = EventType::Console;
    dispatcher.emit(event);
    EXPECT_TRUE(dispatcher.unsubscribe(id));
    dispatcher.emit(event);
    EXPECT_EQ(count, 1);
    EXPECT_FALSE(dispatcher.unsubscribe(id));
}

TEST(Events, HandlersCanCancel) {
    EventDispatcher dispatcher;
    dispatcher.subscribe(EventType::BeforeRequest,
                         [](Event& event) { event.cancelled = true; });
    Event event;
    event.type = EventType::BeforeRequest;
    dispatcher.emit(event);
    EXPECT_TRUE(event.cancelled);
}

TEST(Events, HandlerCountReflectsSubscriptions) {
    EventDispatcher dispatcher;
    dispatcher.subscribe(EventType::Console, [](Event&) {});
    dispatcher.subscribe(EventType::Console, [](Event&) {});
    EXPECT_EQ(dispatcher.handler_count(EventType::Console), 2u);
    dispatcher.clear();
    EXPECT_EQ(dispatcher.handler_count(EventType::Console), 0u);
}

TEST(Events, ReentrantSubscriptionDuringEmit) {
    EventDispatcher dispatcher;
    int count = 0;
    dispatcher.subscribe(EventType::Console,
        [&](Event&) {
            ++count;
            if (count == 1) {
                dispatcher.subscribe(EventType::Console,
                    [&](Event&) { ++count; });
            }
        });
    Event event;
    event.type = EventType::Console;
    dispatcher.emit(event);
    EXPECT_EQ(count, 1);
}

TEST(Events, ReentrantEmitDuringHandler) {
    EventDispatcher dispatcher;
    int count = 0;
    dispatcher.subscribe(EventType::Console,
        [&](Event&) {
            ++count;
            if (count == 1) {
                Event nested;
                nested.type = EventType::Console;
                dispatcher.emit(nested);
            }
        });
    Event event;
    event.type = EventType::Console;
    dispatcher.emit(event);
    EXPECT_EQ(count, 2);
}

TEST(Events, EventDataPayload) {
    EventDispatcher dispatcher;
    std::string received;
    dispatcher.subscribe(EventType::BeforeNavigation,
        [&](Event& event) {
            received = event.url;
        });
    Event event;
    event.type = EventType::BeforeNavigation;
    event.url = "https://example.com/";
    dispatcher.emit(event);
    EXPECT_EQ(received, "https://example.com/");
}

TEST(Events, EventNameField) {
    EventDispatcher dispatcher;
    std::string received;
    dispatcher.subscribe(EventType::Console,
        [&](Event& event) {
            received = event.name;
        });
    Event event;
    event.type = EventType::Console;
    event.name = "custom-console";
    dispatcher.emit(event);
    EXPECT_EQ(received, "custom-console");
}

TEST(Events, SubscribeAllClearedWithClear) {
    EventDispatcher dispatcher;
    int global_count = 0;
    dispatcher.subscribe_all([&](Event&) { ++global_count; });
    dispatcher.clear();
    Event event;
    event.type = EventType::Console;
    dispatcher.emit(event);
    EXPECT_EQ(global_count, 0);
}

TEST(Events, MultipleTypesHandlerCount) {
    EventDispatcher dispatcher;
    dispatcher.subscribe(EventType::Console, [](Event&) {});
    dispatcher.subscribe(EventType::BeforeNavigation, [](Event&) {});
    dispatcher.subscribe(EventType::AfterNavigation, [](Event&) {});
    dispatcher.subscribe(EventType::DocumentCreated, [](Event&) {});
    EXPECT_EQ(dispatcher.handler_count(EventType::Console), 1u);
    EXPECT_EQ(dispatcher.handler_count(EventType::BeforeNavigation), 1u);
    EXPECT_EQ(dispatcher.handler_count(EventType::AfterNavigation), 1u);
    EXPECT_EQ(dispatcher.handler_count(EventType::DocumentCreated), 1u);
}
