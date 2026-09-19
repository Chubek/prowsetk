#include <gtest/gtest.h>

#include <any>
#include <memory>
#include <vector>

#include "prowsetk/anti_bot_detection.hpp"
#include "prowsetk/browser.hpp"
#include "prowsetk/event.hpp"
#include "prowsetk/network_client.hpp"

using prowsetk::AntiBotDetection;
using prowsetk::AntiBotDetector;
using prowsetk::Browser;
using prowsetk::Event;
using prowsetk::EventType;
using prowsetk::HttpRequest;
using prowsetk::HttpResponse;
using prowsetk::MemoryNetworkClient;
using prowsetk::parse_html;

TEST(AntiBotDetection, DetectsCaptchaDocumentMarkers) {
    const auto document = parse_html(R"HTML(
        <html><head><title>Verification required</title></head>
        <body><div class="g-recaptcha" data-sitekey="site"></div></body></html>
    )HTML",
                                     "https://example.com/");

    AntiBotDetector detector;
    const AntiBotDetection detection = detector.inspect_document(*document);

    EXPECT_TRUE(detection.activated);
    EXPECT_EQ(detection.category, "captcha");
    EXPECT_GE(detection.confidence, 0.9);
    EXPECT_FALSE(detection.signals.empty());
}

TEST(AntiBotDetection, DetectsResponseChallengeHeaders) {
    HttpRequest request;
    request.url = "https://example.com/";
    HttpResponse response;
    response.status = 403;
    response.headers.emplace_back("cf-mitigated", "challenge");
    response.body = "<html>verify you are human</html>";

    AntiBotDetector detector;
    const AntiBotDetection detection =
        detector.inspect_response(request, response);

    EXPECT_TRUE(detection.activated);
    EXPECT_EQ(detection.category, "captcha");
    EXPECT_GE(detection.confidence, 0.95);
}

TEST(AntiBotDetection, SessionEmitsAntiBotEventAndStoresLatestResult) {
    Browser browser;
    auto network = std::make_unique<MemoryNetworkClient>();
    network->set_handler([](const HttpRequest&) {
        HttpResponse response;
        response.status = 200;
        response.headers.emplace_back("Content-Type", "text/html");
        response.body = R"HTML(
            <html><body><form><input name="captcha_token"></form></body></html>
        )HTML";
        return response;
    });
    browser.set_network_client(std::move(network));

    auto session = browser.create_session();
    std::vector<Event> events;
    session->events().subscribe(EventType::AntiBotDetected,
                                [&](Event& event) { events.push_back(event); });

    session->navigate("https://example.com/");

    ASSERT_TRUE(session->anti_bot_detection().has_value());
    EXPECT_TRUE(session->anti_bot_detection()->activated);
    ASSERT_FALSE(events.empty());
    EXPECT_EQ(events.back().type, EventType::AntiBotDetected);
    EXPECT_EQ(events.back().name, "captcha");
    EXPECT_TRUE(events.back().payload.has_value());
    EXPECT_NO_THROW(std::any_cast<AntiBotDetection>(events.back().payload));
}
