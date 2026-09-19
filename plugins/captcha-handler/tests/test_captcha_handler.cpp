#include <gtest/gtest.h>

#include <vector>

#include "prowsetk/browser.hpp"
#include "prowsetk/document.hpp"
#include "prowsetk/plugins/captcha_handler.hpp"

namespace captcha = prowsetk::plugins::captcha_handler;

TEST(CaptchaHandler, ExposesEightHandlingMethods) {
    captcha::CaptchaHandlerOptions options;
    options.allow_external_solver = true;
    options.allow_webhook = true;
    options.allow_lua_callback = true;
    options.allow_pre_solved_token = true;
    options.allow_cookie_session_reuse = true;

    const auto methods = captcha::available_methods(options);

    ASSERT_EQ(methods.size(), 8u);
    EXPECT_EQ(methods.front().method, captcha::HandlingMethod::ManualUserPrompt);
    EXPECT_EQ(methods.back().method, captcha::HandlingMethod::AbortAndReport);
    for (const auto& method : methods) {
        EXPECT_FALSE(method.name.empty());
        EXPECT_FALSE(method.description.empty());
        EXPECT_FALSE(method.security_note.empty());
    }
}

TEST(CaptchaHandler, InspectDocumentReturnsDetectionAndPlans) {
    const auto document = prowsetk::parse_html(R"HTML(
        <html><body><div class="cf-turnstile"></div></body></html>
    )HTML",
                                               "https://example.com/");

    const auto result = captcha::inspect_document(*document);

    EXPECT_TRUE(result.detection.activated);
    EXPECT_EQ(result.detection.category, "captcha");
    EXPECT_EQ(result.methods.size(), 8u);
}

TEST(CaptchaHandler, NativePluginLoadsAndEmitsDetectionEvent) {
    prowsetk::Browser browser;
    browser.plugins().load_native(CAPTCHA_HANDLER_PLUGIN_PATH);
    ASSERT_EQ(browser.plugins().initialize_all(), 1u);

    std::vector<prowsetk::Event> events;
    browser.events().subscribe(prowsetk::EventType::AntiBotDetected,
                               [&](prowsetk::Event& event) {
                                   events.push_back(event);
                               });

    auto session = browser.create_session();
    session->load_html(
        "<html><body><div class='h-captcha'></div></body></html>",
        "https://example.com/");

    EXPECT_FALSE(events.empty());
    EXPECT_TRUE(browser.plugins().has_capability("captcha-handling"));
}
