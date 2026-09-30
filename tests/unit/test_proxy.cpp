#include <gtest/gtest.h>
#include <prowsetk/network_client.hpp>
#include <cstdlib>
using namespace prowsetk;
TEST(Proxy, ParsesSchemesAndRedactsCredentials) {
    const auto p = parse_proxy_url("socks5://alice:secret@127.0.0.1:9050");
    EXPECT_EQ(p.scheme, ProxyScheme::Socks5); EXPECT_EQ(p.host, "127.0.0.1"); EXPECT_EQ(p.port, "9050");
    EXPECT_EQ(proxy_to_string(p), "socks5://alice:[REDACTED]@127.0.0.1:9050");
    EXPECT_EQ(proxy_to_string(p, false), "socks5://alice:secret@127.0.0.1:9050");
}
TEST(Proxy, SelectsEnvironmentByTargetScheme) {
    setenv("HTTPS_PROXY", "http://secure-proxy:8443", 1);
    setenv("HTTP_PROXY", "http://plain-proxy:8080", 1);
    auto https = proxy_from_environment("https://example.test/");
    auto http = proxy_from_environment("http://example.test/");
    EXPECT_EQ(https.host, "secure-proxy"); EXPECT_EQ(http.host, "plain-proxy");
    unsetenv("HTTPS_PROXY"); unsetenv("HTTP_PROXY");
}
