#include <gtest/gtest.h>
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
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
    std::filesystem::path certificate;
    std::string old_cert_file;
    bool had_cert_file = false;

    void SetUp() override {
        // The server must tolerate a client rejecting its certificate.
        std::signal(SIGPIPE, SIG_IGN);
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
        std::error_code ignored;
        std::filesystem::remove(certificate, ignored);
    }
    void start(std::string response) {
        worker = std::thread([this, response = std::move(response)] {
            while (!stopping) {
                pollfd pending{listener, POLLIN, 0};
                if (::poll(&pending, 1, 20) <= 0) continue;
                const int fd = ::accept(listener, nullptr, nullptr);
                if (fd < 0) return;
                const timeval timeout{2, 0};
                ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
                ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
                {
                    Connection ssl(SSL_new(context.get()), SSL_free);
                    if (ssl && SSL_set_fd(ssl.get(), fd) == 1 && SSL_accept(ssl.get()) == 1) {
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
} // namespace
