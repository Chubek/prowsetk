#include <gtest/gtest.h>
#include "prowsetk/cdp.hpp"
#include "prowsetk/error.hpp"
#include <deque>
using namespace prowsetk;
namespace {
struct Wire : WebSocket {
    std::deque<std::string> replies;
    std::string sent;
    void send(std::string_view text) override { sent = text; }
    std::string receive() override {
        if (replies.empty()) throw Error(ErrorCode::Timeout, "private-peer-error");
        auto result = replies.front(); replies.pop_front(); return result;
    }
};
}
TEST(CdpClient, CorrelationAndFlattenedEvents) {
    auto wire = std::make_unique<Wire>(); auto* view = wire.get();
    wire->replies = {R"({"method":"Page.loadEventFired","sessionId":"page","params":{}})", R"({"id":1,"sessionId":"page","result":{"value":42}})"};
    CdpClient client(std::move(wire));
    EXPECT_EQ(client.call("Runtime.evaluate", R"({"expression":"6*7"})", "page"), R"({"value":42})");
    EXPECT_NE(view->sent.find("\"sessionId\":\"page\""), std::string::npos);
    EXPECT_EQ(client.take_events().size(), 1u); EXPECT_TRUE(client.take_events().empty());
}
TEST(CdpClient, RejectsMalformedAndUncorrelatedReplies) {
    for (const auto* reply : {R"({"id":2,"result":{}})", R"({"id":1.0000000000000001,"result":{}})", R"({"id":1,"id":1,"result":{}})", R"({"id":1,"sessionId":"wrong","result":{}})", R"({"id":1,"result":[]})", R"({"id":1,"error":{"message":"private-peer-error"}})"}) {
        auto wire = std::make_unique<Wire>(); wire->replies = {reply}; CdpClient client(std::move(wire));
        try { (void)client.call("Browser.getVersion"); FAIL(); }
        catch (const Error& error) { EXPECT_EQ(std::string(error.what()).find("private-peer-error"), std::string::npos); }
        EXPECT_THROW(client.call("Browser.getVersion"), Error);
    }
}
TEST(CdpClient, InvalidParamsDoNotSendAndEventBudgetCloses) {
    auto wire = std::make_unique<Wire>(); auto* view = wire.get();
    for (unsigned i = 0; i < 1025; ++i) wire->replies.push_back(R"({"method":"Page.event","params":{}})");
    CdpClient client(std::move(wire));
    EXPECT_THROW(client.call("Browser.getVersion", "[]"), Error); EXPECT_TRUE(view->sent.empty());
    EXPECT_THROW(client.call("Browser.getVersion"), Error);
    EXPECT_EQ(client.take_events().size(), 1024u);
}
TEST(CdpClient, DefaultNetworkReportsUnsupported) {
    MemoryNetworkClient network;
    EXPECT_THROW(network.open_websocket(HttpRequest{}), Error);
}
TEST(CdpClient, ReentryIsRejectedWithoutCorruptingOuterCall) {
    struct ReentrantWire : WebSocket {
        CdpClient* client = nullptr;
        void send(std::string_view) override { EXPECT_THROW(client->call("Browser.getVersion"), Error); }
        std::string receive() override { return R"({"id":1,"result":{}})"; }
    };
    auto wire = std::make_unique<ReentrantWire>(); auto* view = wire.get(); CdpClient client(std::move(wire));
    view->client = &client;
    EXPECT_EQ(client.call("Browser.getVersion"), "{}");
}
