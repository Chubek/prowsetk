// Hermetic unit tests for the OpenCode bridge client and DOM sanitizer.
// No network, no filesystem writes; the host transport is a
// MemoryNetworkClient with scripted handlers.

#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "opencode_bridge.hpp"
#include "prowsetk/browser.hpp"
#include "prowsetk/error.hpp"
#include "prowsetk/network_client.hpp"

namespace bridge = prowsetk::plugins::opencode_bridge;

namespace {

bridge::BridgeConfig config() {
    bridge::BridgeConfig cfg;
    cfg.base_url = "http://127.0.0.1:4096";
    cfg.max_requests = 16;
    return cfg;
}

prowsetk::HttpResponse json_response(std::string body, int status = 200) {
    prowsetk::HttpResponse response;
    response.status = status;
    response.body = std::move(body);
    return response;
}

}  // namespace

TEST(OpencodeBridgeClient, CreateSessionExtractsId) {
    auto network = std::make_unique<prowsetk::MemoryNetworkClient>();
    network->set_handler([](const prowsetk::HttpRequest& request) {
        EXPECT_EQ(request.url, "http://127.0.0.1:4096/session");
        EXPECT_EQ(request.method, "POST");
        return json_response(R"({"id":"sess-1"})");
    });
    bridge::OpenCodeClient client(*network, config());
    EXPECT_EQ(client.create_session(), "sess-1");
    EXPECT_EQ(client.request_count(), 1u);
}

TEST(OpencodeBridgeClient, PromptSendsTextAndTools) {
    auto network = std::make_unique<prowsetk::MemoryNetworkClient>();
    network->set_handler([](const prowsetk::HttpRequest& request) {
        EXPECT_EQ(request.url, "http://127.0.0.1:4096/session/s1/message");
        EXPECT_NE(request.body.find("hello"), std::string::npos);
        EXPECT_NE(request.body.find("click"), std::string::npos);
        return json_response(R"({"text":"done"})");
    });
    bridge::OpenCodeClient client(*network, config());
    EXPECT_EQ(client.prompt("s1", "hello", {"click"}), "done");
}

TEST(OpencodeBridgeClient, PromptAsyncAndAbort) {
    auto network = std::make_unique<prowsetk::MemoryNetworkClient>();
    network->set_handler([](const prowsetk::HttpRequest& request) {
        if (request.url.find("prompt_async") != std::string::npos) {
            return json_response(R"({"operation_id":"op-9"})");
        }
        EXPECT_NE(request.url.find("abort"), std::string::npos);
        return json_response(R"({})");
    });
    bridge::OpenCodeClient client(*network, config());
    EXPECT_EQ(client.prompt_async("s1", "crawl"), "op-9");
    EXPECT_NO_THROW(client.abort("s1"));
    EXPECT_EQ(client.request_count(), 2u);
}

TEST(OpencodeBridgeClient, StreamEventsParsesSsePayloads) {
    auto network = std::make_unique<prowsetk::MemoryNetworkClient>();
    network->set_handler([](const prowsetk::HttpRequest& request) {
        EXPECT_EQ(request.method, "GET");
        EXPECT_NE(request.url.find("/event"), std::string::npos);
        return json_response("event: message\ndata: {\"t\":1}\n\ndata: [DONE]\n\ndata: {\"t\":2}\n");
    });
    bridge::OpenCodeClient client(*network, config());
    const auto events = client.stream_events("s1");
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[0], "{\"t\":1}");
    EXPECT_EQ(events[1], "{\"t\":2}");
}

TEST(OpencodeBridgeSse, MultilineDataLinesJoinIntoOneEvent) {
    const auto events = bridge::parse_sse_events("data: {\"a\":1,\ndata: \"b\":2}\n\n");
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0], "{\"a\":1,\n\"b\":2}");
}

TEST(OpencodeBridgeSse, DoneAloneIsDroppedButMergedDataIsKept) {
    EXPECT_TRUE(bridge::parse_sse_events("data: [DONE]\n\n").empty());
    const auto events = bridge::parse_sse_events("data: [DONE]\ndata: {\"t\":2}\n\n");
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0], "[DONE]\n{\"t\":2}");
}

TEST(OpencodeBridgeClient, StreamEventsEncodesSessionId) {
    auto network = std::make_unique<prowsetk::MemoryNetworkClient>();
    network->set_handler([](const prowsetk::HttpRequest& request) {
        EXPECT_NE(request.url.find("sessionId=s1%26x%3D1"), std::string::npos);
        return json_response("data: {}\n\n");
    });
    bridge::OpenCodeClient client(*network, config());
    const auto events = client.stream_events("s1&x=1");
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0], "{}");
}

TEST(OpencodeBridgeClient, DotSegmentSessionIdsAreRejected) {
    auto network = std::make_unique<prowsetk::MemoryNetworkClient>();
    network->set_handler([](const prowsetk::HttpRequest&) {
        ADD_FAILURE() << "no request must be sent";
        return json_response("{}");
    });
    bridge::OpenCodeClient client(*network, config());
    EXPECT_THROW(client.prompt(".", "hi"), prowsetk::Error);
    EXPECT_THROW(client.prompt("..", "hi"), prowsetk::Error);
    EXPECT_THROW(client.abort(".."), prowsetk::Error);
}

TEST(OpencodeBridgeClient, BasicAuthHeaderUsesConfiguredCredentials) {
    auto network = std::make_unique<prowsetk::MemoryNetworkClient>();
    network->set_handler([](const prowsetk::HttpRequest& request) {
        bool found = false;
        for (const auto& [name, value] : request.headers) {
            if (name == "Authorization") {
                // "user:pass" base64 is "dXNlcjpwYXNz".
                EXPECT_EQ(value, "Basic dXNlcjpwYXNz");
                found = true;
            }
        }
        EXPECT_TRUE(found);
        return json_response(R"({"id":"s"})");
    });
    auto cfg = config();
    cfg.username = "user";
    cfg.password = "pass";
    bridge::OpenCodeClient client(*network, cfg);
    EXPECT_EQ(client.create_session(), "s");
}

TEST(OpencodeBridgeClient, RedirectsAreRejected) {
    auto network = std::make_unique<prowsetk::MemoryNetworkClient>();
    network->set_handler([](const prowsetk::HttpRequest&) {
        prowsetk::HttpResponse response = json_response(R"({"id":"s"})", 301);
        response.final_url = "http://127.0.0.1:4096/session";
        return response;
    });
    bridge::OpenCodeClient client(*network, config());
    EXPECT_THROW(client.create_session(), prowsetk::Error);
}

TEST(OpencodeBridgeClient, RequestBudgetIsEnforced) {
    auto network = std::make_unique<prowsetk::MemoryNetworkClient>();
    network->set_handler([](const prowsetk::HttpRequest&) { return json_response(R"({"id":"s"})"); });
    auto cfg = config();
    cfg.max_requests = 1;
    bridge::OpenCodeClient client(*network, cfg);
    EXPECT_EQ(client.create_session(), "s");
    EXPECT_THROW(client.create_session(), prowsetk::Error);
}

TEST(OpencodeBridgeClient, ScrapeWithPromptValidatesJsonObject) {
    auto network = std::make_unique<prowsetk::MemoryNetworkClient>();
    network->set_handler([](const prowsetk::HttpRequest& request) {
        if (request.url == "http://127.0.0.1:4096/session") return json_response(R"({"id":"s"})");
        // Sanitized prompt must not carry raw script bodies or base64 payloads.
        EXPECT_EQ(request.body.find("evil()"), std::string::npos);
        EXPECT_EQ(request.body.find("aGVsbG8="), std::string::npos);
        return json_response(R"({"text":"{\"title\":\"Hi\"}"})");
    });
    bridge::OpenCodeClient outer(*network, config());
    const std::string session = outer.create_session();
    bridge::PageSnapshot snapshot;
    snapshot.url = "https://example.test/";
    snapshot.title = "Hi";
    snapshot.html =
        "<html><head><script>evil()</script></head><body><main><h1>Hi</h1>"
        "<img src=\"data:image/png;base64,aGVsbG8=\"></main></body></html>";
    snapshot.text = "Hi";
    const std::string answer =
        outer.scrape_with_prompt(session, snapshot, "Get the heading", "{\"title\":\"string\"}");
    EXPECT_EQ(answer, "{\"title\":\"Hi\"}");
}

TEST(OpencodeBridgeClient, ScrapeWithPromptRejectsNonObjectAnswer) {
    auto network = std::make_unique<prowsetk::MemoryNetworkClient>();
    network->set_handler([](const prowsetk::HttpRequest& request) {
        if (request.url == "http://127.0.0.1:4096/session") return json_response(R"({"id":"s"})");
        return json_response(R"({"text":"[1,2]"})");
    });
    bridge::OpenCodeClient client(*network, config());
    bridge::PageSnapshot snapshot;
    snapshot.html = "<p>Hi</p>";
    EXPECT_THROW(client.scrape_with_prompt("s", snapshot, "Go", "{\"a\":\"b\"}"), prowsetk::Error);
}

TEST(OpencodeBridgeDom, SanitizerStripsHeavyContentKeepsSemantics) {
    const std::string html =
        "<main><nav><a href=\"/x\">Go</a></nav><article data-id=\"7\"><h1>T</h1>"
        "<script>evil()</script><img src=\"data:image/png;base64,aGVsbG8=\">"
        "<p onclick=\"evil()\">Body</p></article></main>";
    const std::string clean = bridge::sanitize_html(html);
    EXPECT_EQ(clean.find("evil()"), std::string::npos);
    EXPECT_EQ(clean.find("aGVsbG8="), std::string::npos);
    EXPECT_EQ(clean.find("onclick"), std::string::npos);
    EXPECT_NE(clean.find("<main>"), std::string::npos);
    EXPECT_NE(clean.find("<nav>"), std::string::npos);
    EXPECT_NE(clean.find("data-id"), std::string::npos);
}

TEST(OpencodeBridgeDom, LargeDocumentsFallBackToSummary) {
    std::string html = "<article>";
    for (int i = 0; i < 20000; ++i) html += "<p>paragraph text here</p>";
    html += "</article>";
    const std::string clean = bridge::sanitize_html(html, 4096);
    EXPECT_LT(clean.size(), 8192u);
    EXPECT_NE(clean.find("text:"), std::string::npos);
}

TEST(OpencodeBridgeJson, EscapeAndExtractRoundTrip) {
    const std::string escaped = bridge::json_escape("a\"b\nc\\");
    EXPECT_EQ(escaped, "\"a\\\"b\\nc\\\\\"");
    std::string out;
    EXPECT_TRUE(bridge::json_extract_string(R"({"id":"s-1","n":2})", "id", out));
    EXPECT_EQ(out, "s-1");
    EXPECT_FALSE(bridge::json_extract_string(R"({"n":2})", "id", out));
}

TEST(OpencodeBridgeJson, KeysInsideStringValuesDoNotMatch) {
    std::string out;
    // The only real key is "text"; the echoed key-like text (including an
    // escaped \"id\": sequence) inside the value must not shadow it.
    const std::string body = R"({"text":"say \"id\": \"fake\" loudly","id":"real"})";
    EXPECT_TRUE(bridge::json_extract_string(body, "id", out));
    EXPECT_EQ(out, "real");
    EXPECT_TRUE(bridge::json_extract_string(body, "text", out));
    EXPECT_EQ(out, "say \"id\": \"fake\" loudly");
    // A document with the key only inside a string has no match.
    EXPECT_FALSE(bridge::json_extract_string(R"({"text":"the \"id\" key"})", "id", out));
    // Non-string values are skipped, later string occurrences still match.
    EXPECT_TRUE(bridge::json_extract_string(R"({"id":7,"id":"s"})", "id", out));
    EXPECT_EQ(out, "s");
}

TEST(OpencodeBridgeJson, SurrogatePairsCombineAndLoneSurrogatesReplace) {
    std::string out;
    EXPECT_TRUE(bridge::json_extract_string(R"({"t":"\ud83d\ude00"})", "t", out));
    EXPECT_EQ(out, "\xf0\x9f\x98\x80");  // U+1F600
    EXPECT_TRUE(bridge::json_extract_string(R"({"t":"a\ud800b"})", "t", out));
    EXPECT_EQ(out, "a\xef\xbf\xbd"
                     "b");  // lone high surrogate -> U+FFFD
    // Malformed escapes fail closed instead of yielding partial text.
    EXPECT_FALSE(bridge::json_extract_string(R"({"t":"\udcxx"})", "t", out));
}

TEST(OpencodeBridgeConfig, InvalidBaseUrlRejected) {
    auto cfg = config();
    cfg.base_url = "http://127.0.0.1:4096/path?q=1";
    EXPECT_THROW(bridge::validate_config(cfg), prowsetk::Error);
    cfg = config();
    cfg.base_url = "";
    EXPECT_THROW(bridge::validate_config(cfg), prowsetk::Error);
}

TEST(OpencodeBridgeConfig, RemoteHttpRequiresExplicitOptIn) {
    auto cfg = config();
    cfg.base_url = "http://opencode.lan:4096";
    EXPECT_THROW(bridge::validate_config(cfg), prowsetk::Error);
    cfg.allow_remote_http = true;
    EXPECT_NO_THROW(bridge::validate_config(cfg));
    // Loopback HTTP and any HTTPS stay allowed by default.
    cfg = config();
    cfg.base_url = "http://localhost:4096";
    EXPECT_NO_THROW(bridge::validate_config(cfg));
    cfg.base_url = "https://opencode.example.com/v1";
    EXPECT_NO_THROW(bridge::validate_config(cfg));
}

TEST(OpencodeBridgeCleanupPrompt, CarriesSubtractiveContractAndBudgets) {
    const std::string prompt = bridge::build_endpoint_cleanup_prompt(
        R"([{"url":"https://a.test/api","method":"get"}])", "prefer v2");
    EXPECT_NE(prompt.find("NEVER invent"), std::string::npos);
    EXPECT_NE(prompt.find("prefer v2"), std::string::npos);
    EXPECT_NE(prompt.find("https://a.test/api"), std::string::npos);
    EXPECT_THROW(bridge::build_endpoint_cleanup_prompt("", "x"), prowsetk::Error);
    EXPECT_THROW(bridge::build_endpoint_cleanup_prompt("[{}]", "x", 0), prowsetk::Error);
    // Oversized endpoint lists fail closed before any request is built.
    EXPECT_THROW(bridge::build_endpoint_cleanup_prompt(std::string(300000, 'x'), "", 1024),
                 prowsetk::Error);
}

TEST(OpencodeBridgeUtil, PercentEncodeLeavesUnreservedIntact) {
    EXPECT_EQ(bridge::percent_encode("s1-_.~"), "s1-_.~");
    EXPECT_EQ(bridge::percent_encode("a&b=c"), "a%26b%3Dc");
}

namespace {

bridge::BridgeConfig v2_config() {
    bridge::BridgeConfig cfg;
    cfg.base_url = "http://127.0.0.1:4096";
    cfg.api_prefix = "/api";
    cfg.max_requests = 64;
    cfg.prompt_wait_ms = 5000;
    return cfg;
}

}  // namespace

TEST(OpencodeBridgeV2, CreateSessionReadsNestedDataId) {
    auto network = std::make_unique<prowsetk::MemoryNetworkClient>();
    network->set_handler([](const prowsetk::HttpRequest& request) {
        EXPECT_EQ(request.url, "http://127.0.0.1:4096/api/session");
        return json_response(R"({"data":{"id":"ses_1"}})");
    });
    bridge::OpenCodeClient client(*network, v2_config());
    EXPECT_EQ(client.create_session(), "ses_1");
}

TEST(OpencodeBridgeV2, PromptWaitsForCompletedAssistant) {
    auto network = std::make_unique<prowsetk::MemoryNetworkClient>();
    int calls = 0;
    network->set_handler([&calls](const prowsetk::HttpRequest& request) {
        ++calls;
        if (request.url == "http://127.0.0.1:4096/api/session/s1/prompt") {
            EXPECT_EQ(request.method, "POST");
            EXPECT_NE(request.body.find("clean"), std::string::npos);
            return json_response(R"({"data":{"id":"msg_9","type":"user"}})");
        }
        EXPECT_EQ(request.url, "http://127.0.0.1:4096/api/session/s1/message");
        if (calls == 1) return json_response(R"({"data":[]})");
        if (calls == 3) {
            return json_response(R"({"data":[
                {"id":"msg_2","type":"assistant","time":{"created":1},
                 "content":[{"type":"text","text":"partial"}]}]})");
        }
        return json_response(R"({"data":[
            {"id":"msg_2","type":"assistant","time":{"created":1,"completed":2},
             "content":[{"type":"reasoning","text":"thinking"},
                        {"type":"text","text":"answer"},
                        {"type":"text","text":"more"}]}]})");
    });
    bridge::OpenCodeClient client(*network, v2_config());
    // Reasoning parts are excluded; text parts join with newlines.
    EXPECT_EQ(client.prompt("s1", "clean"), "answer\nmore");
    EXPECT_GE(calls, 4);
}

TEST(OpencodeBridgeV2, PromptTimesOutWhenAgentNeverReplies) {
    auto network = std::make_unique<prowsetk::MemoryNetworkClient>();
    int polls = 0;
    network->set_handler([&polls](const prowsetk::HttpRequest& request) {
        if (request.url.find("/prompt") != std::string::npos) {
            return json_response("{\"data\":{\"id\":\"msg_1\"}}");
        }
        ++polls;
        return json_response("{\"data\":[]}");
    });
    auto cfg = v2_config();
    cfg.prompt_wait_ms = 100;
    bridge::OpenCodeClient client(*network, cfg);
    try {
        client.prompt("s1", "clean");
        FAIL() << "expected timeout";
    } catch (const prowsetk::Error& error) {
        EXPECT_EQ(error.code(), prowsetk::ErrorCode::Timeout);
    }
    EXPECT_GE(polls, 1);
    EXPECT_LE(polls, 3);
}

TEST(OpencodeBridgeV2, PromptRejectsToolsWithoutSending) {
    auto network = std::make_unique<prowsetk::MemoryNetworkClient>();
    int calls = 0;
    network->set_handler([&calls](const prowsetk::HttpRequest&) {
        ++calls;
        return json_response("{}");
    });
    bridge::OpenCodeClient client(*network, v2_config());
    EXPECT_THROW(client.prompt("s1", "clean", {"click"}), prowsetk::Error);
    EXPECT_EQ(calls, 0);
}

TEST(OpencodeBridgeV2, FailedAgentRunThrowsWithoutAnswer) {
    auto network = std::make_unique<prowsetk::MemoryNetworkClient>();
    network->set_handler([](const prowsetk::HttpRequest& request) {
        if (request.url.find("/prompt") != std::string::npos) {
            return json_response(R"({"data":{"id":"msg_1"}})");
        }
        return json_response(R"({"data":[
            {"id":"run_1","type":"idle","outcome":"failed"}]})");
    });
    bridge::OpenCodeClient client(*network, v2_config());
    EXPECT_THROW(client.prompt("s1", "clean"), prowsetk::Error);
}

TEST(OpencodeBridgeV2, PromptAsyncReturnsMessageIdAndAbortHitsInterrupt) {
    auto network = std::make_unique<prowsetk::MemoryNetworkClient>();
    network->set_handler([](const prowsetk::HttpRequest& request) {
        if (request.url.find("/prompt") != std::string::npos) {
            return json_response(R"({"data":{"id":"msg_7"}})");
        }
        EXPECT_NE(request.url.find("/interrupt"), std::string::npos);
        return json_response(R"({"interrupted":true})");
    });
    bridge::OpenCodeClient client(*network, v2_config());
    EXPECT_EQ(client.prompt_async("s1", "clean"), "msg_7");
    EXPECT_NO_THROW(client.abort("s1"));
}

TEST(OpencodeBridgeV2, MalformedMessageListFailsClosed) {
    auto network = std::make_unique<prowsetk::MemoryNetworkClient>();
    network->set_handler([](const prowsetk::HttpRequest&) {
        return json_response("<html>login</html>");
    });
    bridge::OpenCodeClient client(*network, v2_config());
    EXPECT_THROW(client.prompt("s1", "clean"), prowsetk::Error);
}

TEST(OpencodeBridgeConfig, ApiPrefixAndWaitBoundsAreValidated) {
    auto cfg = config();
    cfg.api_prefix = "/v3";
    EXPECT_THROW(bridge::validate_config(cfg), prowsetk::Error);
    cfg = config();
    cfg.api_prefix = "/api";
    EXPECT_NO_THROW(bridge::validate_config(cfg));
    cfg.prompt_wait_ms = -1;
    EXPECT_THROW(bridge::validate_config(cfg), prowsetk::Error);
    cfg.prompt_wait_ms = 600001;
    EXPECT_THROW(bridge::validate_config(cfg), prowsetk::Error);
}

TEST(OpencodeBridgeJsonParse, NestedPathsAndMalformedInput) {
    bridge::JsonValue root;
    ASSERT_TRUE(bridge::json_parse(R"({"data":{"id":"ses_9","n":2}})", root));
    std::string out;
    EXPECT_TRUE(bridge::json_find_string_at_path(root, {"data", "id"}, out));
    EXPECT_EQ(out, "ses_9");
    EXPECT_FALSE(bridge::json_find_string_at_path(root, {"data", "n"}, out));
    EXPECT_FALSE(bridge::json_find_string_at_path(root, {"missing", "id"}, out));
    EXPECT_TRUE(bridge::json_parse(R"({"a":1})", root));
    EXPECT_FALSE(bridge::json_parse(R"({"a":)", root));
    EXPECT_FALSE(bridge::json_parse(R"([1,])", root));
    EXPECT_FALSE(bridge::json_parse(R"({"a":1} trailing)", root));
    std::string deep(40, '[');
    deep += std::string(40, ']');
    EXPECT_FALSE(bridge::json_parse(deep, root));
}

TEST(OpencodeBridgeJsonParse, AssistantMessageExtraction) {
    bridge::JsonValue root;
    ASSERT_TRUE(bridge::json_parse(R"({"data":[
        {"id":"m0","type":"user","text":"hi"},
        {"id":"m1","type":"assistant","time":{"created":1},
         "content":[{"type":"reasoning","text":"hmm"},{"type":"text","text":"A"}]},
        {"id":"m2","type":"assistant","time":{"created":2,"completed":3},
         "content":[{"type":"text","text":"B"}]},
        {"id":"r1","type":"idle","outcome":"succeeded"}]})", root));
    const auto messages = bridge::parse_assistant_messages(root);
    ASSERT_EQ(messages.size(), 3u);
    EXPECT_EQ(messages[0].id, "m1");
    EXPECT_FALSE(messages[0].completed);
    EXPECT_EQ(messages[0].text, "A");
    EXPECT_TRUE(messages[1].completed);
    EXPECT_EQ(messages[1].text, "B");
    EXPECT_TRUE(messages[2].idle);
    EXPECT_TRUE(messages[2].succeeded);
}
