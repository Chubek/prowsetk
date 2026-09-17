#include <gtest/gtest.h>

#include "prowsetk/web_platform.hpp"

TEST(WebPlatform, ReportsEngineCapabilitiesHonestly) {
    const auto platform = prowsetk::default_web_platform();

    EXPECT_TRUE(platform.supports("dom"));
    EXPECT_EQ(platform.classification("dom"),
              prowsetk::ImplementationClass::FullyImplemented);

    EXPECT_TRUE(platform.supports("xpath"));
    EXPECT_NE(platform.classification("xpath"),
              prowsetk::ImplementationClass::Unsupported);

    EXPECT_TRUE(platform.supports("canvas"));
    EXPECT_EQ(platform.classification("canvas"),
              prowsetk::ImplementationClass::DummyImplementation);

    EXPECT_FALSE(platform.supports("WebGL"));
    EXPECT_EQ(platform.classification("WebGL"),
              prowsetk::ImplementationClass::Unsupported);
}

TEST(WebPlatform, DoesNotClaimUninstalledJavaScriptBindings) {
    // Page-script host bindings that are not installed must be reported
    // honestly (README "Compatibility Policy").
    const auto platform = prowsetk::default_web_platform();
    for (const char* api : {"window", "document-js", "location", "navigator",
                            "URL", "URLSearchParams", "fetch", "XMLHttpRequest",
                            "timers", "cookies", "localStorage",
                            "sessionStorage"}) {
        EXPECT_FALSE(platform.supports(api)) << "must not claim " << api;
    }
}

TEST(WebPlatform, ConsoleBindingIsAvailable) {
    const auto platform = prowsetk::default_web_platform();
    EXPECT_TRUE(platform.supports("console"));
    EXPECT_EQ(platform.classification("console"),
              prowsetk::ImplementationClass::FullyImplemented);
}

TEST(WebPlatform, UnknownApiIsUnsupported) {
    const auto platform = prowsetk::default_web_platform();
    EXPECT_EQ(platform.classification("unknown-api"),
              prowsetk::ImplementationClass::Unsupported);
    EXPECT_FALSE(platform.supports("unknown-api"));
}

TEST(WebPlatform, NetworkAndStorageClassifications) {
    const auto platform = prowsetk::default_web_platform();
    EXPECT_TRUE(platform.supports("network"));
    EXPECT_EQ(platform.classification("network"),
              prowsetk::ImplementationClass::ImplementedWithRestrictions);
    EXPECT_TRUE(platform.supports("storage"));
    EXPECT_EQ(platform.classification("storage"),
              prowsetk::ImplementationClass::FullyImplemented);
}

TEST(WebPlatform, EndpointExtractionClassification) {
    const auto platform = prowsetk::default_web_platform();
    EXPECT_TRUE(platform.supports("endpoint-extraction"));
    EXPECT_EQ(platform.classification("endpoint-extraction"),
              prowsetk::ImplementationClass::PartiallyImplemented);
}

TEST(WebPlatform, EventsClassification) {
    const auto platform = prowsetk::default_web_platform();
    EXPECT_TRUE(platform.supports("events"));
    EXPECT_EQ(platform.classification("events"),
              prowsetk::ImplementationClass::FullyImplemented);
}

TEST(WebPlatform, CssSelectorsPartiallyImplemented) {
    const auto platform = prowsetk::default_web_platform();
    EXPECT_TRUE(platform.supports("css-selectors"));
    EXPECT_EQ(platform.classification("css-selectors"),
              prowsetk::ImplementationClass::PartiallyImplemented);
}

TEST(WebPlatform, JavaScriptPartiallyImplemented) {
    const auto platform = prowsetk::default_web_platform();
    EXPECT_TRUE(platform.supports("javascript"));
    EXPECT_EQ(platform.classification("javascript"),
              prowsetk::ImplementationClass::PartiallyImplemented);
}

TEST(WebPlatform, DummyCanvasClassification) {
    const auto platform = prowsetk::default_web_platform();
    EXPECT_TRUE(platform.supports("canvas"));
    EXPECT_EQ(platform.classification("canvas"),
              prowsetk::ImplementationClass::DummyImplementation);
    EXPECT_EQ(platform.classification("WebGL"),
              prowsetk::ImplementationClass::Unsupported);
}