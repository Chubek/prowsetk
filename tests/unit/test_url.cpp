#include <gtest/gtest.h>

#include "prowsetk/url.hpp"

using prowsetk::parse_url;
using prowsetk::resolve_url;
using prowsetk::normalize_url;
using prowsetk::Url;

TEST(Url, ParsesAbsoluteUrl) {
    const Url url = parse_url("https://user@example.com:8443/a/b?x=1#frag");
    EXPECT_EQ(url.scheme, "https");
    EXPECT_EQ(url.userinfo, "user");
    EXPECT_EQ(url.host, "example.com");
    EXPECT_EQ(url.port, "8443");
    EXPECT_EQ(url.path, "/a/b");
    EXPECT_EQ(url.query, "x=1");
    EXPECT_EQ(url.fragment, "frag");
    EXPECT_TRUE(url.has_authority);
}

TEST(Url, ParsesRelativeReference) {
    const Url url = parse_url("../sibling?q=2");
    EXPECT_TRUE(url.scheme.empty());
    EXPECT_FALSE(url.has_authority);
    EXPECT_EQ(url.path, "../sibling");
    EXPECT_EQ(url.query, "q=2");
}

TEST(Url, ResolvesRelativePaths) {
    EXPECT_EQ(resolve_url("https://example.com/a/b/c", "d"),
              "https://example.com/a/b/d");
    EXPECT_EQ(resolve_url("https://example.com/a/b/c", "../d"),
              "https://example.com/a/d");
    EXPECT_EQ(resolve_url("https://example.com/a/b/c", "/root"),
              "https://example.com/root");
}

TEST(Url, ResolvesAbsoluteAndNetworkPath) {
    EXPECT_EQ(resolve_url("https://example.com/a", "http://other.test/x"),
              "http://other.test/x");
    EXPECT_EQ(resolve_url("https://example.com/a", "//cdn.test/lib.js"),
              "https://cdn.test/lib.js");
}

TEST(Url, KeepsQueryWhenReferenceHasNoPath) {
    EXPECT_EQ(resolve_url("https://example.com/a?x=1", "#frag"),
              "https://example.com/a?x=1#frag");
    EXPECT_EQ(resolve_url("https://example.com/a?x=1", "?y=2"),
              "https://example.com/a?y=2");
    EXPECT_EQ(resolve_url("https://example.com/a?x=1", "?"),
              "https://example.com/a?");
    EXPECT_EQ(resolve_url("https://example.com/a?x=1", "#"),
              "https://example.com/a?x=1#");
}

TEST(Url, NormalizesDefaultPortsAndDotSegments) {
    EXPECT_EQ(normalize_url("HTTP://Example.COM:80/a/./b/../c"),
              "http://example.com/a/c");
    EXPECT_EQ(normalize_url("https://example.com:443"), "https://example.com/");
}

TEST(Url, OriginExcludesPath) {
    const Url url = parse_url("https://example.com:8443/a/b");
    EXPECT_EQ(url.origin(), "https://example.com:8443");
}
