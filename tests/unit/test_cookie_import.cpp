#include <gtest/gtest.h>

#include "prowsetk/cookie_import.hpp"
#include "prowsetk/error.hpp"
#include "prowsetk/storage.hpp"
#include "prowsetk/url.hpp"

TEST(CookieImport, ImportsFirefoxCookieArray) {
    prowsetk::MemoryStorage storage;
    const auto result = prowsetk::import_cookies_json(
        storage.cookies(),
        R"JSON([
          {
            "name": "sessionid",
            "value": "abc123",
            "domain": ".booking.com",
            "hostOnly": false,
            "path": "/",
            "secure": true,
            "httpOnly": true,
            "sameSite": "lax",
            "expirationDate": 1893456000.25
          }
        ])JSON");

    EXPECT_EQ(result.imported, 1u);
    EXPECT_EQ(result.skipped, 0u);
    const auto header =
        storage.cookies().cookie_header(prowsetk::parse_url("https://admin.booking.com/"));
    EXPECT_EQ(header, "sessionid=abc123");
    const auto cookies =
        storage.cookies().get(prowsetk::parse_url("https://admin.booking.com/"));
    ASSERT_EQ(cookies.size(), 1u);
    EXPECT_TRUE(cookies[0].http_only);
    EXPECT_EQ(cookies[0].same_site, "lax");
    EXPECT_EQ(cookies[0].expires_unix.value(), 1893456000);
}

TEST(CookieImport, ImportsStorageStateCookiesObject) {
    prowsetk::MemoryStorage storage;
    const auto result = prowsetk::import_cookies_json(
        storage.cookies(),
        R"JSON({
          "cookies": [
            {
              "name": "host",
              "value": "only",
              "url": "https://example.test/account",
              "path": "/account",
              "secure": true
            }
          ]
        })JSON");

    EXPECT_EQ(result.imported, 1u);
    EXPECT_EQ(storage.cookies().cookie_header(
                  prowsetk::parse_url("https://example.test/account/settings")),
              "host=only");
    EXPECT_TRUE(storage.cookies()
                    .cookie_header(prowsetk::parse_url("https://other.test/account"))
                    .empty());
}

TEST(CookieImport, RejectsUnsupportedRootShape) {
    prowsetk::MemoryStorage storage;
    EXPECT_THROW(prowsetk::import_cookies_json(storage.cookies(), R"JSON({"x": []})JSON"),
                 prowsetk::Error);
}
