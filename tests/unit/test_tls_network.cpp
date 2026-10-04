#include <gtest/gtest.h>
#include <atomic>
#include <array>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "prowsetk/error.hpp"
#include "prowsetk/network_client.hpp"

namespace {
using Context = std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)>;
using Connection = std::unique_ptr<SSL, decltype(&SSL_free)>;

// In-process loopback TLS peer: no Internet, external commands, or sleeps.
class TlsNetwork : public ::testing::Test {
protected:
    Context context{nullptr, SSL_CTX_free};
    int listener = -1;
    unsigned short port = 0;
    std::thread worker;
    std::atomic<bool> stopping{false};
    std::string received;
    std::string proxy_request;
    std::filesystem::path certificate;
    std::string old_cert_file;
    bool had_cert_file = false;
    static constexpr std::array proxy_names{"HTTP_PROXY", "http_proxy", "HTTPS_PROXY", "https_proxy",
        "ALL_PROXY", "all_proxy", "NO_PROXY", "no_proxy"};
    std::array<std::optional<std::string>, proxy_names.size()> proxy_environment;

    void SetUp() override {
        // The server must tolerate a client rejecting its certificate.
        std::signal(SIGPIPE, SIG_IGN);
        for (std::size_t i = 0; i < proxy_names.size(); ++i) {
            if (const auto* value = std::getenv(proxy_names[i])) proxy_environment[i] = value;
            ::unsetenv(proxy_names[i]);
        }
        if (const char* previous = std::getenv("SSL_CERT_FILE")) {
            old_cert_file = previous;
            had_cert_file = true;
        }
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        certificate = std::filesystem::path(TEST_BINARY_DIR) /
            (std::string("tls-") + info->name() + ".pem");
        std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(
            EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "P-256"), EVP_PKEY_free);
        std::unique_ptr<X509, decltype(&X509_free)> cert(X509_new(), X509_free);
        ASSERT_TRUE(key);
        ASSERT_TRUE(cert);
        ASSERT_EQ(X509_set_version(cert.get(), 2), 1);
        ASSERT_EQ(ASN1_INTEGER_set(X509_get_serialNumber(cert.get()), 1), 1);
        ASSERT_NE(X509_gmtime_adj(X509_getm_notBefore(cert.get()), -60), nullptr);
        ASSERT_NE(X509_gmtime_adj(X509_getm_notAfter(cert.get()), 3600), nullptr);
        ASSERT_EQ(X509_set_pubkey(cert.get(), key.get()), 1);
        auto* subject = X509_get_subject_name(cert.get());
        ASSERT_EQ(X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC,
            reinterpret_cast<const unsigned char*>("localhost"), -1, -1, 0), 1);
        ASSERT_EQ(X509_set_issuer_name(cert.get(), subject), 1);
        for (const auto& entry : {std::pair{NID_subject_alt_name, "DNS:localhost"},
                                 std::pair{NID_basic_constraints, "critical,CA:TRUE"}}) {
            std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)> extension(
                X509V3_EXT_conf_nid(nullptr, nullptr, entry.first, entry.second),
                X509_EXTENSION_free);
            ASSERT_TRUE(extension);
            ASSERT_EQ(X509_add_ext(cert.get(), extension.get(), -1), 1);
        }
        ASSERT_GT(X509_sign(cert.get(), key.get(), EVP_sha256()), 0);
        std::unique_ptr<BIO, decltype(&BIO_free)> pem(
            BIO_new_file(certificate.c_str(), "w"), BIO_free);
        ASSERT_TRUE(pem);
        ASSERT_EQ(PEM_write_bio_X509(pem.get(), cert.get()), 1);
        pem.reset();
        ASSERT_EQ(::setenv("SSL_CERT_FILE", certificate.c_str(), 1), 0);
        context.reset(SSL_CTX_new(TLS_server_method()));
        ASSERT_TRUE(context);
        ASSERT_EQ(SSL_CTX_use_certificate(context.get(), cert.get()), 1);
        ASSERT_EQ(SSL_CTX_use_PrivateKey(context.get(), key.get()), 1);
        listener = ::socket(AF_INET, SOCK_STREAM, 0);
        ASSERT_GE(listener, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ASSERT_EQ(::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
        socklen_t size = sizeof(address);
        ASSERT_EQ(::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &size), 0);
        port = ntohs(address.sin_port);
        ASSERT_EQ(::listen(listener, 1), 0);
    }
    void TearDown() override {
        stopping = true;
        if (worker.joinable()) worker.join();
        if (listener >= 0) ::close(listener);
        if (had_cert_file) ::setenv("SSL_CERT_FILE", old_cert_file.c_str(), 1);
        else ::unsetenv("SSL_CERT_FILE");
        for (std::size_t i = 0; i < proxy_names.size(); ++i) {
            if (proxy_environment[i]) ::setenv(proxy_names[i], proxy_environment[i]->c_str(), 1);
            else ::unsetenv(proxy_names[i]);
        }
        std::error_code ignored;
        std::filesystem::remove(certificate, ignored);
    }
    void start(std::string response, bool tunnel = false, bool encrypted_proxy = false,
               std::string connect_response = "HTTP/1.1 200 Connection established\r\n\r\n") {
        worker = std::thread([this, response = std::move(response), tunnel, encrypted_proxy, connect_response = std::move(connect_response)] {
            while (!stopping) {
                pollfd pending{listener, POLLIN, 0};
                if (::poll(&pending, 1, 20) <= 0) continue;
                const int fd = ::accept(listener, nullptr, nullptr);
                if (fd < 0) return;
                const timeval timeout{2, 0};
                ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
                ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
                {
                    Connection outer(nullptr, SSL_free);
                    if (encrypted_proxy) {
                        outer.reset(SSL_new(context.get()));
                        if (!outer || SSL_set_fd(outer.get(), fd) != 1 || SSL_accept(outer.get()) != 1) {
                            ::close(fd);
                            return;
                        }
                    }
                    if (tunnel) {
                        char byte;
                        while (proxy_request.find("\r\n\r\n") == std::string::npos && proxy_request.size() < 65536) {
                            const auto count = outer ? SSL_read(outer.get(), &byte, 1) : ::recv(fd, &byte, 1, 0);
                            if (count <= 0) break;
                            proxy_request += byte;
                        }
                        // Deliberately fragmented proxy headers exercise partial reads.
                        for (char byte_out : connect_response) {
                            if (outer) SSL_write(outer.get(), &byte_out, 1);
                            else ::send(fd, &byte_out, 1, 0);
                        }
                        if (!connect_response.starts_with("HTTP/1.1 200 ")) {
                            ::close(fd);
                            return;
                        }
                    }
                    Connection ssl(SSL_new(context.get()), SSL_free);
                    if (outer && tunnel && ssl) {
                        std::unique_ptr<BIO, decltype(&BIO_free)> filter(BIO_new(BIO_f_ssl()), BIO_free);
                        if (filter) {
                            BIO_set_ssl(filter.get(), outer.get(), BIO_NOCLOSE);
                            SSL_set_bio(ssl.get(), filter.get(), filter.get());
                            filter.release();
                        }
                    } else if (outer) ssl = std::move(outer);
                    else if (ssl) SSL_set_fd(ssl.get(), fd);
                    if (ssl && ((!tunnel && encrypted_proxy) || SSL_accept(ssl.get()) == 1)) {
                        char bytes[4096];
                        while (received.find("\r\n\r\n") == std::string::npos) {
                            const int count = SSL_read(ssl.get(), bytes, sizeof(bytes));
                            if (count <= 0) break;
                            received.append(bytes, static_cast<std::size_t>(count));
                        }
                        std::size_t sent = 0;
                        SSL_write_ex(ssl.get(), response.data(), response.size(), &sent);
                        SSL_shutdown(ssl.get());
                    }
                }
                ::close(fd);
                return;
            }
        });
    }
    prowsetk::HttpRequest request(const std::string& host = "localhost") {
        prowsetk::HttpRequest result;
        result.url = "https://" + host + ":" + std::to_string(port) + "/api/hotels";
        result.timeout_ms = 2000;
        return result;
    }
};

TEST_F(TlsNetwork, VerifiedHttpsAndChunkedResponse) {
    start("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nOK\r\n0\r\n\r\n");
    auto client = prowsetk::make_socket_network_client();
    const auto response = client->send(request());
    EXPECT_EQ(response.status, 200);
    EXPECT_EQ(response.body, "OK");
    worker.join();
    EXPECT_NE(received.find("GET /api/hotels HTTP/1.1"), std::string::npos);
    EXPECT_NE(received.find("Host: localhost:" + std::to_string(port)), std::string::npos);
}
TEST_F(TlsNetwork, RejectsHostnameMismatchBeforeSendingCredentials) {
    start("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
    auto req = request("127.0.0.1");
    req.body = "password=fixture-only";
    EXPECT_THROW(prowsetk::make_socket_network_client()->send(req), prowsetk::Error);
    worker.join();
    EXPECT_TRUE(received.empty());
}
TEST_F(TlsNetwork, RejectsUntrustedCertificateBeforeSendingCredentials) {
    ::setenv("SSL_CERT_FILE", "/nonexistent/prowsetk-ca.pem", 1);
    start("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
    EXPECT_THROW(prowsetk::make_socket_network_client()->send(request()), prowsetk::Error);
    worker.join();
    EXPECT_TRUE(received.empty());
}
TEST_F(TlsNetwork, RejectsTruncatedBody) {
    start("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nshort");
    EXPECT_THROW(prowsetk::make_socket_network_client()->send(request()), prowsetk::Error);
}
TEST_F(TlsNetwork, EnforcesResponseLimit) {
    start("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\n0123456789");
    auto req = request();
    req.max_response_bytes = 5;
    EXPECT_THROW(prowsetk::make_socket_network_client()->send(req), prowsetk::Error);
}

TEST_F(TlsNetwork, HttpsConnectUsesProxyAuthButNeverForwardsItToOrigin) {
    start("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nOK", true);
    auto req = request();
    req.url += "?limit=2&token=origin-private#fragment-private";
    const auto proxy_url = "http://alice:secret@localhost:" + std::to_string(port);
    ::setenv("HTTPS_PROXY", proxy_url.c_str(), 1);
    req.headers.emplace_back("Proxy-Authorization", "caller-private");
    ASSERT_EQ(prowsetk::make_socket_network_client()->send(req).body, "OK");
    worker.join();
    EXPECT_TRUE(proxy_request.starts_with("CONNECT localhost:" + std::to_string(port) + " HTTP/1.1\r\n"));
    EXPECT_NE(proxy_request.find("Proxy-Authorization: Basic YWxpY2U6c2VjcmV0\r\n"), std::string::npos);
    EXPECT_EQ(proxy_request.find("origin-private"), std::string::npos);
    EXPECT_EQ(received.find("Proxy-Authorization"), std::string::npos);
    EXPECT_EQ(received.find("caller-private"), std::string::npos);
    EXPECT_EQ(received.find("fragment-private"), std::string::npos);
    EXPECT_TRUE(received.starts_with("GET /api/hotels?limit=2&token=origin-private HTTP/1.1\r\n"));
}

TEST_F(TlsNetwork, RejectsProxy407EvenWhenHeadersContainA200) {
    start("", true, false, "HTTP/1.1 407 Authentication required\r\nX-Status: 200\r\n\r\n");
    auto req = request();
    req.proxy = prowsetk::parse_proxy_url("http://localhost:" + std::to_string(port));
    try { prowsetk::make_socket_network_client()->send(req); FAIL(); }
    catch (const prowsetk::Error& error) {
        EXPECT_EQ(error.code(), prowsetk::ErrorCode::NetworkError);
        EXPECT_EQ(std::string(error.what()), "proxy CONNECT rejected (HTTP 407)");
    }
}

TEST_F(TlsNetwork, VerifiedHttpsProxyCarriesHttpAbsoluteForm) {
    start("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nOK", false, true);
    auto req = request();
    req.url = "http://origin.invalid/api/hotels?limit=2";
    req.proxy = prowsetk::parse_proxy_url("https://localhost:" + std::to_string(port));
    ASSERT_EQ(prowsetk::make_socket_network_client()->send(req).body, "OK");
    worker.join();
    EXPECT_TRUE(received.starts_with("GET http://origin.invalid/api/hotels?limit=2 HTTP/1.1\r\n"));
}

TEST_F(TlsNetwork, VerifiedHttpsProxyTunnelsVerifiedOriginTls) {
    start("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nOK", true, true);
    auto req = request();
    req.proxy = prowsetk::parse_proxy_url("https://alice:secret@localhost:" + std::to_string(port));
    ASSERT_EQ(prowsetk::make_socket_network_client()->send(req).body, "OK");
    worker.join();
    EXPECT_NE(proxy_request.find("Proxy-Authorization: Basic YWxpY2U6c2VjcmV0\r\n"), std::string::npos);
    EXPECT_TRUE(received.starts_with("GET /api/hotels HTTP/1.1\r\n"));
    EXPECT_EQ(received.find("Proxy-Authorization"), std::string::npos);
}

TEST_F(TlsNetwork, HttpsProxyCertificateIsVerifiedBeforeSendingCredentials) {
    start("", true, true);
    auto req = request();
    req.proxy = prowsetk::parse_proxy_url("https://alice:secret@127.0.0.1:" + std::to_string(port));
    EXPECT_THROW(prowsetk::make_socket_network_client()->send(req), prowsetk::Error);
    worker.join();
    EXPECT_TRUE(proxy_request.empty());
    EXPECT_TRUE(received.empty());
}
} // namespace
