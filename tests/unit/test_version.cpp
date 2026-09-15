#include <gtest/gtest.h>

#include "prowsetk/version.hpp"

TEST(Version, ReportsSemanticVersion) {
    EXPECT_EQ(std::string(prowsetk::version()), "0.1.0");
    EXPECT_EQ(PROWSETK_VERSION_MAJOR, 0);
    EXPECT_EQ(PROWSETK_VERSION_MINOR, 1);
    EXPECT_EQ(PROWSETK_VERSION_PATCH, 0);
}
