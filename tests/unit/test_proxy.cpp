#include <gtest/gtest.h>
#include <prowsetk/network_client.hpp>
#include <prowsetk/error.hpp>
#include <array>
#include <cstdlib>
#include <optional>
using namespace prowsetk;

namespace {
class ProxyEnvironment : public testing::Test {
protected:
    static constexpr std::array names{"HTTPS_PROXY", "https_proxy", "HTTP_PROXY", "http_proxy",
        "ALL_PROXY", "all_proxy", "NO_PROXY", "no_proxy"};
    std::array<std::optional<std::string>, names.size()> saved;
    void SetUp() override {
        for (std::size_t i = 0; i < names.size(); ++i) {
            if (const auto* value = std::getenv(names[i])) saved[i] = value;
            unsetenv(names[i]);
        }
    }
    void TearDown() override {
        for (std::size_t i = 0; i < names.size(); ++i) {
            if (saved[i]) setenv(names[i], saved[i]->c_str(), 1);
            else unsetenv(names[i]);
        }
    }
};
}
TEST(Proxy, ParsesSchemesAndRedactsCredentials) {
    const auto p = parse_proxy_url("socks5://alice:secret@127.0.0.1:9050");
    EXPECT_EQ(p.scheme, ProxyScheme::Socks5); EXPECT_EQ(p.host, "127.0.0.1"); EXPECT_EQ(p.port, "9050");
    EXPECT_EQ(proxy_to_string(p), "socks5://alice:[REDACTED]@127.0.0.1:9050");
    EXPECT_EQ(proxy_to_string(p, false), "socks5://alice:secret@127.0.0.1:9050");
}
TEST_F(ProxyEnvironment, SelectsEnvironmentByTargetScheme) {
    setenv("HTTPS_PROXY", "http://secure-proxy:8443", 1);
    setenv("HTTP_PROXY", "http://plain-proxy:8080", 1);
    auto https = proxy_from_environment("https://example.test/");
    auto http = proxy_from_environment("http://example.test/");
    EXPECT_EQ(https.host, "secure-proxy"); EXPECT_EQ(http.host, "plain-proxy");
}

TEST_F(ProxyEnvironment, UsesLowercaseAliasesAndAllProxyWithoutMixingSchemes) {
    setenv("https_proxy", "http://secure-proxy:8443", 1);
    setenv("http_proxy", "http://plain-proxy:8080", 1);
    setenv("ALL_PROXY", "socks5://fallback-proxy:1080", 1);
    EXPECT_EQ(proxy_from_environment("https://example.test/").host, "secure-proxy");
    EXPECT_EQ(proxy_from_environment("http://example.test/").host, "plain-proxy");
    unsetenv("http_proxy");
    EXPECT_EQ(proxy_from_environment("http://example.test/").host, "fallback-proxy");
    unsetenv("https_proxy");
    setenv("HTTP_PROXY", "http://plain-proxy:8080", 1);
    EXPECT_EQ(proxy_from_environment("https://example.test/").host, "fallback-proxy");
}

TEST_F(ProxyEnvironment, HonorsNoProxyHostsSuffixesPortsAndIpv6) {
    setenv("HTTP_PROXY", "http://proxy.test:8080", 1);
    setenv("HTTPS_PROXY", "http://proxy.test:8080", 1);
    setenv("NO_PROXY", " localhost, 127.0.0.1, ::1, .booking.com, port.test:443, [::2]:4096 ", 1);
    for (const auto* url : {"http://localhost:4096/", "http://127.0.0.1/", "http://[::1]/",
                           "https://booking.com/", "https://admin.booking.com/", "https://port.test/", "http://[::2]:4096/"}) {
        EXPECT_FALSE(proxy_from_environment(url).enabled()) << url;
    }
    for (const auto* url : {"http://notbooking.com/", "http://booking.com.other/", "http://port.test/", "http://[::2]/"}) {
        EXPECT_TRUE(proxy_from_environment(url).enabled()) << url;
    }
    unsetenv("NO_PROXY");
    setenv("no_proxy", "*", 1);
    EXPECT_FALSE(proxy_from_environment("https://other.test/").enabled());
}

TEST(Proxy, DecodesCredentialsAndIpv6WithoutEchoingInvalidValues) {
    const auto proxy = parse_proxy_url("http://alice:p%40ss%3Aword@[::1]:8080/");
    EXPECT_EQ(proxy.host, "::1");
    EXPECT_EQ(proxy.password, "p@ss:word");
    EXPECT_EQ(proxy_to_string(proxy), "http://alice:[REDACTED]@[::1]:8080");
    EXPECT_EQ(parse_proxy_url(proxy_to_string(proxy, false)).password, proxy.password);
    for (const auto* url : {"http://private:secret@host:garbage", "http://host:0", "http://host:65536",
                           "http://private:secret@host:80/path", "http://host:80\r\n", "http://user:%0a@host:80"}) {
        try { parse_proxy_url(url); FAIL() << "expected invalid proxy"; }
        catch (const Error& error) {
            EXPECT_EQ(std::string(error.what()).find("private"), std::string::npos);
            EXPECT_EQ(std::string(error.what()).find("secret"), std::string::npos);
        }
    }
}
