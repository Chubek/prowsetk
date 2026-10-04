#include <gtest/gtest.h>
#include "prowsetk/plugins/opencode_marionette.hpp"
#include "prowsetk/error.hpp"
#include <fstream>
namespace m = prowsetk::plugins::opencode_marionette;
namespace b = prowsetk::plugins::opencode_bridge;
TEST(MarionetteDecisions, StrictPolicyAndChoice) {
    auto policy = m::parse_decisions(R"({"version":1,"goal":"search","actions":[{"id":"search","kind":"click","selector":"#search"}]})");
    EXPECT_EQ(policy.actions.size(), 1u);
    EXPECT_EQ(m::parse_choice(R"({"action":"search"})", policy), "search");
    EXPECT_EQ(m::parse_choice(R"({"action":"stop"})", policy), "stop");
    for (const auto* bad : {R"({"action":"evaluate"})", R"({"action":"stop","code":"x"})",
         R"({"action":"stop","action":"search"})", R"({"action":"stop"} trailing)", R"({"action":null})"}) {
        EXPECT_THROW(m::parse_choice(bad, policy), prowsetk::Error);
    }
}
TEST(MarionetteDecisions, RejectsInvalidBoundsAndActions) {
    for (const auto* bad : {R"({"version":2,"goal":"x","actions":[]})",
         R"({"version":1,"goal":"x","max_steps":0,"actions":[]})",
         R"({"version":1,"goal":"x","max_steps":1.5,"actions":[]})",
         R"({"version":1,"goal":"x","max_steps":65,"actions":[]})",
         R"({"version":1,"goal":"x","goal":"y","actions":[]})",
         R"({"version":1,"goal":"x","actions":[{"id":"stop","kind":"click","selector":"b"}]})",
         R"({"version":1,"goal":"x","actions":[{"id":"x","kind":"evaluate","value":"code"}]})",
         R"({"version":1,"goal":"x","actions":[{"id":"x","kind":"click","selector":"b","value":"secret"}]})"}) {
        EXPECT_THROW(m::parse_decisions(bad), prowsetk::Error);
    }
    EXPECT_THROW(m::parse_decisions(std::string(65537, ' ')), prowsetk::Error);
}
TEST(MarionetteDecisions, FileLoadingAndMissingFile) {
    const auto path = std::filesystem::current_path() / "marionette-unit-policy.json";
    { std::ofstream file(path); file << R"({"version":1,"goal":"x","actions":[]})"; }
    EXPECT_EQ(m::load_decisions(path).goal, "x");
    std::filesystem::remove(path);
    EXPECT_THROW(m::load_decisions(path), prowsetk::Error);
}
TEST(MarionetteOpenCode, DocumentedPartsProtocolAndAgentFailure) {
    prowsetk::MemoryNetworkClient network;
    network.set_handler([](const prowsetk::HttpRequest& request) {
        EXPECT_EQ(request.url, "http://127.0.0.1:4096/session/test/message");
        EXPECT_NE(request.body.find("\"parts\""), std::string::npos);
        EXPECT_NE(request.body.find("\"bash\":false"), std::string::npos);
        return prowsetk::HttpResponse{200, {}, R"({"info":{"role":"assistant"},"parts":[{"type":"reasoning","text":"ignored"},{"type":"text","text":"answer"}]})", {}, {}};
    });
    b::OpenCodeClient client(network, b::BridgeConfig{});
    EXPECT_EQ(client.prompt_message("test", "prompt"), "answer");
    network.set_handler([](const auto&) { return prowsetk::HttpResponse{200, {}, R"({"info":{"error":{"data":"private"}},"parts":[{"type":"text","text":"answer"}]})", {}, {}}; });
    EXPECT_THROW(client.prompt_message("test", "prompt"), prowsetk::Error);
    EXPECT_THROW(client.prompt_message("..", "prompt"), prowsetk::Error);
}

#include "prowsetk/plugins/scrape_endpoints.hpp"
TEST(MarionetteDiscovery, AllPathsCandidateOptionPreservesDefaultAndDropsAssets) {
    namespace scrape = prowsetk::plugins::scrape_endpoints;
    const auto document = prowsetk::parse_html("<a href='/custom-endpoint'>api</a><a href='/static/bundle.js'>asset</a>", "https://page.test/");
    scrape::ScrapeEndpointsOptions options;
    options.require_api_pattern = false;
    EXPECT_TRUE(scrape::scrape_from_document(*document, options).endpoints.empty());
    options.scrape_all_paths = true;
    const auto result = scrape::scrape_from_document(*document, options);
    ASSERT_EQ(result.endpoints.size(), 1u);
    EXPECT_EQ(result.endpoints[0].url, "https://page.test/custom-endpoint");
}

TEST(MarionetteDecisions, LiteralControlBytesFailStrictJson) {
    EXPECT_THROW(m::parse_decisions("{\"version\":1,\"goal\":\"a\nb\",\"actions\":[]}"), prowsetk::Error);
}
