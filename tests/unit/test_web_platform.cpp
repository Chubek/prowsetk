#include <gtest/gtest.h>

#include "prowsetk/web_platform.hpp"

TEST(WebPlatform, ReportsDocumentAndHeadlessCompatibility) {
    const auto platform = prowsetk::default_web_platform();
    EXPECT_TRUE(platform.supports("document"));
    EXPECT_EQ(platform.classification("document"),
              prowsetk::ImplementationClass::FullyImplemented);
    EXPECT_TRUE(platform.supports("canvas"));
    EXPECT_EQ(platform.classification("canvas"),
              prowsetk::ImplementationClass::DummyImplementation);
    EXPECT_FALSE(platform.supports("WebGL"));
    EXPECT_EQ(platform.classification("unknown-api"),
              prowsetk::ImplementationClass::Unsupported);
}
