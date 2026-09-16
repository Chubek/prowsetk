#include <gtest/gtest.h>

#include "prowsetk/capability.hpp"
#include "prowsetk/web_platform.hpp"

using prowsetk::CapabilitySet;
using prowsetk::ImplementationClass;
using prowsetk::default_web_platform;

TEST(Capability, ReportsOnlyUsableCapabilities) {
    CapabilitySet capabilities;
    capabilities.set("html", ImplementationClass::FullyImplemented);
    capabilities.set("canvas", ImplementationClass::DummyImplementation);
    capabilities.set("webgl", ImplementationClass::Unsupported);

    EXPECT_TRUE(capabilities.has("html"));
    EXPECT_TRUE(capabilities.has("canvas"));
    EXPECT_FALSE(capabilities.has("webgl"));
    EXPECT_FALSE(capabilities.has("missing"));
}

TEST(Capability, ClassificationAndLookup) {
    CapabilitySet capabilities;
    capabilities.set("fetch", ImplementationClass::ImplementedWithRestrictions,
                     "host-mediated");

    EXPECT_EQ(capabilities.classification("fetch"),
              ImplementationClass::ImplementedWithRestrictions);
    EXPECT_EQ(capabilities.classification("nope"),
              ImplementationClass::Unsupported);
    const auto* found = capabilities.find("fetch");
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->notes, "host-mediated");
}

TEST(Capability, ToStringIsStable) {
    EXPECT_STREQ(prowsetk::to_string(ImplementationClass::FullyImplemented),
                 "fully-implemented");
    EXPECT_STREQ(prowsetk::to_string(ImplementationClass::Unsupported),
                 "unsupported");
}

TEST(WebPlatform, DocumentsApiSurface) {
    const auto platform = default_web_platform();
    // The C++ DOM engine is a real capability; the JavaScript fetch host
    // binding is not installed and must not be advertised.
    EXPECT_TRUE(platform.supports("dom"));
    EXPECT_FALSE(platform.supports("fetch"));
    EXPECT_FALSE(platform.supports("WebGL"));
    EXPECT_EQ(platform.classification("canvas"),
              ImplementationClass::DummyImplementation);
}
