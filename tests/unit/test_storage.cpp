#include <gtest/gtest.h>

#include "prowsetk/storage.hpp"
#include "prowsetk/url.hpp"

using prowsetk::Cookie;
using prowsetk::MemoryStorage;
using prowsetk::parse_url;

TEST(Storage, CookiesAreScopedByDomainAndPath) {
    MemoryStorage storage;
    const auto origin = parse_url("https://example.com/app/page");

    Cookie cookie;
    cookie.name = "session";
    cookie.value = "abc";
    cookie.path = "/app";
    storage.cookies().set(origin, cookie);

    EXPECT_EQ(storage.cookies().get(origin).size(), 1u);
    EXPECT_EQ(storage.cookies().cookie_header(origin), "session=abc");

    const auto other_path = parse_url("https://example.com/elsewhere");
    EXPECT_TRUE(storage.cookies().get(other_path).empty());

    const auto other_host = parse_url("https://other.test/app");
    EXPECT_TRUE(storage.cookies().get(other_host).empty());
}

TEST(Storage, SecureCookiesRequireHttps) {
    MemoryStorage storage;
    Cookie cookie;
    cookie.name = "secure";
    cookie.value = "1";
    cookie.secure = true;
    storage.cookies().set(parse_url("https://example.com/"), cookie);

    EXPECT_EQ(storage.cookies().get(parse_url("https://example.com/")).size(), 1u);
    EXPECT_TRUE(storage.cookies().get(parse_url("http://example.com/")).empty());
}

TEST(Storage, CookieHeaderJoinsMultipleCookies) {
    MemoryStorage storage;
    Cookie first;
    first.name = "a";
    first.value = "1";
    Cookie second;
    second.name = "b";
    second.value = "2";
    const auto origin = parse_url("https://example.com/");
    storage.cookies().set(origin, first);
    storage.cookies().set(origin, second);

    const std::string header = storage.cookies().cookie_header(origin);
    EXPECT_NE(header.find("a=1"), std::string::npos);
    EXPECT_NE(header.find("b=2"), std::string::npos);
}

TEST(Storage, SessionsAreIsolated) {
    MemoryStorage storage;
    storage.local_storage("one").set("key", "first");
    storage.local_storage("two").set("key", "second");

    ASSERT_TRUE(storage.local_storage("one").get("key").has_value());
    EXPECT_EQ(storage.local_storage("one").get("key").value(), "first");
    EXPECT_EQ(storage.local_storage("two").get("key").value(), "second");

    storage.release_session("one");
    EXPECT_FALSE(storage.local_storage("one").get("key").has_value());
    EXPECT_TRUE(storage.local_storage("two").get("key").has_value());
}

TEST(Storage, KeyValueStoreOperations) {
    MemoryStorage storage;
    auto& store = storage.session_storage("s");
    store.set("k", "v");
    EXPECT_TRUE(store.get("k").has_value());
    EXPECT_TRUE(store.remove("k"));
    EXPECT_FALSE(store.remove("k"));
    EXPECT_FALSE(store.get("k").has_value());
}

TEST(Storage, CookiePathMatchingUsesPathBoundaries) {
    MemoryStorage storage;
    Cookie cookie;
    cookie.name = "scoped";
    cookie.path = "/app";
    storage.cookies().set(parse_url("https://example.com/app/start"), cookie);

    EXPECT_EQ(storage.cookies().get(parse_url("https://example.com/app"))
                  .size(),
              1u);
    EXPECT_EQ(storage.cookies().get(parse_url("https://example.com/app/next"))
                  .size(),
              1u);
    EXPECT_TRUE(storage.cookies().get(parse_url("https://example.com/apple"))
                    .empty());
}

TEST(Storage, DomainCookiesNormalizeAndHostOnlyCookiesDoNotLeak) {
    MemoryStorage storage;
    Cookie domain_cookie;
    domain_cookie.name = "shared";
    domain_cookie.domain = ".Example.COM";
    domain_cookie.host_only = false;
    storage.cookies().set(parse_url("https://www.example.com/"),
                          domain_cookie);

    Cookie host_cookie;
    host_cookie.name = "host";
    storage.cookies().set(parse_url("https://www.example.com/"), host_cookie);

    EXPECT_EQ(storage.cookies().get(parse_url("https://api.example.com/"))
                  .size(),
              1u);
    EXPECT_EQ(storage.cookies().get(parse_url("https://www.example.com/"))
                  .size(),
              2u);
}

TEST(Storage, SettingSameCookieReplacesAndExpiredCookieDeletes) {
    MemoryStorage storage;
    const auto origin = parse_url("https://example.com/");
    Cookie cookie;
    cookie.name = "session";
    cookie.value = "first";
    storage.cookies().set(origin, cookie);

    cookie.value = "second";
    storage.cookies().set(origin, cookie);
    ASSERT_EQ(storage.cookies().get(origin).size(), 1u);
    EXPECT_EQ(storage.cookies().get(origin)[0].value, "second");

    cookie.expires_unix = 1;
    storage.cookies().set(origin, cookie);
    EXPECT_TRUE(storage.cookies().get(origin).empty());
}

TEST(Storage, CookieHeaderOrdersMoreSpecificPathsFirst) {
    MemoryStorage storage;
    const auto origin = parse_url("https://example.com/app/page");
    Cookie broad;
    broad.name = "scope";
    broad.value = "broad";
    broad.path = "/";
    storage.cookies().set(origin, broad);
    Cookie narrow = broad;
    narrow.value = "narrow";
    narrow.path = "/app";
    storage.cookies().set(origin, narrow);

    EXPECT_EQ(storage.cookies().cookie_header(origin),
              "scope=narrow; scope=broad");
}
