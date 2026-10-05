#include "prowsetk/network_client.hpp"
#include "prowsetk/cdp.hpp"
#include "protocol_json.hpp"
#include <chrono>
#include <array>
#include <optional>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <map>
#include <mutex>
#include <memory>
#include <sstream>
#include <cstring>

#ifdef PROWSETK_HAVE_OPENSSL
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#endif

#include "prowsetk/error.hpp"
#include "prowsetk/url.hpp"

#if defined(__unix__) || defined(__APPLE__)
#include <cerrno>
#include <cstddef>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace prowsetk {

std::string to_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    });
    return value;
}


namespace {
std::string trim(std::string value) {
    const auto not_space = [](unsigned char c) { return std::isspace(c) == 0; };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
    return value;
}

const char* environment_value(const char* upper, const char* lower) {
    const char* value = std::getenv(upper);
    return value && *value ? value : std::getenv(lower);
}

std::string unbracket_host(std::string host) {
    if (host.size() > 2 && host.front() == '[' && host.back() == ']') return host.substr(1, host.size() - 2);
    return host;
}

bool proxy_excluded(const Url& target) {
    const char* excluded = environment_value("NO_PROXY", "no_proxy");
    if (!excluded || !*excluded) return false;
    auto host = unbracket_host(to_lower(target.host));
    if (host.ends_with('.')) host.pop_back();
    const auto port = target.port.empty() ? (target.scheme == "https" ? "443" : "80") : target.port;
    std::istringstream entries(excluded);
    std::string entry;
    while (std::getline(entries, entry, ',')) {
        entry = to_lower(trim(std::move(entry)));
        if (entry == "*") return true;
        std::string required_port;
        if (entry.starts_with('[')) {
            const auto close = entry.find(']');
            if (close == std::string::npos) continue;
            if (close + 1 < entry.size()) {
                if (entry[close + 1] != ':') continue;
                required_port = entry.substr(close + 2);
            }
            entry = entry.substr(1, close - 1);
        } else if (const auto colon = entry.find(':'); colon != std::string::npos && entry.find(':', colon + 1) == std::string::npos) {
            required_port = entry.substr(colon + 1);
            entry.resize(colon);
        }
        if (!required_port.empty() && required_port != port) continue;
        if (entry.starts_with("*.")) entry.erase(0, 2);
        else if (entry.starts_with('.')) entry.erase(0, 1);
        if (entry.ends_with('.')) entry.pop_back();
        if (entry.empty()) continue;
        if (host == entry || host.ends_with("." + entry)) return true;
    }
    return false;
}

std::string decode_proxy_credential(std::string_view value) {
    const auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string result;
    for (std::size_t i = 0; i < value.size(); ++i) {
        char c = value[i];
        if (c == '%') {
            if (i + 2 >= value.size() || hex(value[i + 1]) < 0 || hex(value[i + 2]) < 0)
                throw Error(ErrorCode::InvalidUrl, "invalid proxy credential encoding");
            c = static_cast<char>((hex(value[i + 1]) << 4) | hex(value[i + 2]));
            i += 2;
        }
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f)
            throw Error(ErrorCode::InvalidUrl, "invalid proxy credentials");
        result += c;
    }
    return result;
}

std::string encode_proxy_credential(std::string_view value) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string encoded;
    for (unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~') encoded += static_cast<char>(c);
        else {
            encoded += '%';
            encoded += hex[c >> 4];
            encoded += hex[c & 15];
        }
    }
    return encoded;
}
}  // namespace

ProxyConfig parse_proxy_url(std::string_view value) {
    if (value.empty()) return {};
    if (value.size() > 8192 || std::any_of(value.begin(), value.end(), [](unsigned char c) { return c <= 0x20 || c == 0x7f; }))
        throw Error(ErrorCode::InvalidUrl, "invalid proxy URL");
    const auto scheme_end = value.find("://");
    if (scheme_end == std::string_view::npos) throw Error(ErrorCode::InvalidUrl, "proxy URL requires a scheme");
    auto scheme = to_lower(std::string(value.substr(0, scheme_end)));
    ProxyConfig proxy;
    if (scheme == "http") proxy.scheme = ProxyScheme::Http;
    else if (scheme == "https") proxy.scheme = ProxyScheme::Https;
    else if (scheme == "socks5" || scheme == "socks5h") proxy.scheme = ProxyScheme::Socks5;
    else throw Error(ErrorCode::Unsupported, "unsupported proxy scheme");
    auto authority = value.substr(scheme_end + 3);
    const auto slash = authority.find('/');
    if (slash != std::string_view::npos) {
        if (authority.substr(slash) != "/") throw Error(ErrorCode::InvalidUrl, "proxy URL must not contain a path");
        authority = authority.substr(0, slash);
    }
    if (authority.find_first_of("?#") != std::string_view::npos) throw Error(ErrorCode::InvalidUrl, "invalid proxy URL");
    std::string userinfo;
    const auto at = authority.rfind('@');
    if (at != std::string_view::npos) { userinfo = std::string(authority.substr(0, at)); authority = authority.substr(at + 1); }
    const auto colon = authority.rfind(':');
    if (colon == std::string_view::npos || colon == 0 || colon + 1 >= authority.size()) throw Error(ErrorCode::InvalidUrl, "proxy URL requires host:port");
    proxy.host = unbracket_host(to_lower(std::string(authority.substr(0, colon))));
    proxy.port = std::string(authority.substr(colon + 1));
    if (!userinfo.empty()) {
        const auto sep = userinfo.find(':');
        proxy.username = decode_proxy_credential(std::string_view(userinfo).substr(0, sep));
        if (sep != std::string::npos) proxy.password = decode_proxy_credential(std::string_view(userinfo).substr(sep + 1));
    }
    unsigned port = 0;
    const auto parsed = std::from_chars(proxy.port.data(), proxy.port.data() + proxy.port.size(), port);
    if (parsed.ec != std::errc{} || parsed.ptr != proxy.port.data() + proxy.port.size() || !port || port > 65535)
        throw Error(ErrorCode::InvalidUrl, "invalid proxy port");
    if (proxy.host.empty() || proxy.host.size() > 255 || !std::all_of(proxy.host.begin(), proxy.host.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '.' || c == '-' || c == ':';
    })) throw Error(ErrorCode::InvalidUrl, "invalid proxy host");
    if (proxy.scheme == ProxyScheme::Socks5 && (proxy.username.size() > 255 || proxy.password.size() > 255))
        throw Error(ErrorCode::InvalidUrl, "proxy credentials exceed SOCKS5 limits");
    return proxy;
}
ProxyConfig proxy_from_environment(std::string_view target_url) {
    const auto target = parse_url(target_url);
    if (proxy_excluded(target)) return {};
    const char* value = target.scheme == "https" ? environment_value("HTTPS_PROXY", "https_proxy") :
        environment_value("HTTP_PROXY", "http_proxy");
    if (!value || !*value) value = environment_value("ALL_PROXY", "all_proxy");
    if (!value || !*value) return {};
    return parse_proxy_url(value);
}
std::string proxy_to_string(const ProxyConfig& proxy, bool redact) {
    if (!proxy.enabled()) return {};
    const char* scheme = proxy.scheme == ProxyScheme::Socks5 ? "socks5" : proxy.scheme == ProxyScheme::Https ? "https" : "http";
    std::string result = std::string(scheme) + "://";
    if (!proxy.username.empty()) result += encode_proxy_credential(proxy.username) + ":" +
        (redact ? "[REDACTED]" : encode_proxy_credential(proxy.password)) + "@";
    const auto host = proxy.host.find(':') == std::string::npos ? proxy.host : "[" + proxy.host + "]";
    return result + host + ":" + proxy.port;
}

std::string HttpResponse::header(std::string_view name) const {
    const std::string wanted = to_lower(std::string(name));
    for (const auto& [key, value] : headers) {
        if (to_lower(key) == wanted) {
            return value;
        }
    }
    return {};
}

struct MemoryNetworkClient::Impl {
    std::mutex mutex;
    std::map<std::string, HttpResponse> responses;
    std::function<HttpResponse(const HttpRequest&)> handler;
    std::vector<HttpRequest> requests;
};

MemoryNetworkClient::MemoryNetworkClient() : impl_(std::make_unique<Impl>()) {}
MemoryNetworkClient::~MemoryNetworkClient() = default;

void MemoryNetworkClient::set_response(std::string url, HttpResponse response) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->responses[std::move(url)] = std::move(response);
}

void MemoryNetworkClient::set_handler(
    std::function<HttpResponse(const HttpRequest&)> handler) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->handler = std::move(handler);
}

HttpResponse MemoryNetworkClient::send(const HttpRequest& request) {
    std::function<HttpResponse(const HttpRequest&)> handler;
    HttpResponse response;
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->requests.push_back(request);
        handler = impl_->handler;
        const auto it = impl_->responses.find(request.url);
        if (it != impl_->responses.end()) {
            response = it->second;
            found = true;
        }
    }
    if (handler) {
        response = handler(request);
        found = true;
    }
    if (!found) {
        throw Error(ErrorCode::NotFound,
                    "no response registered for " + request.url);
    }
    if (response.final_url.empty()) {
        response.final_url = request.url;
    }
    return response;
}

const std::vector<HttpRequest>& MemoryNetworkClient::requests() const {
    return impl_->requests;
}

#if defined(__unix__) || defined(__APPLE__)
namespace {

std::string proxy_authorization(const ProxyConfig& proxy) {
    if (proxy.username.empty()) return {};
    static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const auto plain = proxy.username + ":" + proxy.password;
    std::string encoded;
    for (std::size_t i = 0; i < plain.size(); i += 3) {
        const auto a = static_cast<unsigned char>(plain[i]);
        const auto b = i + 1 < plain.size() ? static_cast<unsigned char>(plain[i + 1]) : 0;
        const auto c = i + 2 < plain.size() ? static_cast<unsigned char>(plain[i + 2]) : 0;
        encoded += alphabet[a >> 2];
        encoded += alphabet[((a & 3) << 4) | (b >> 4)];
        encoded += i + 1 < plain.size() ? alphabet[((b & 15) << 2) | (c >> 6)] : '=';
        encoded += i + 2 < plain.size() ? alphabet[c & 63] : '=';
    }
    return "\r\nProxy-Authorization: Basic " + encoded;
}

std::string read_status_line_error() {
    return "malformed HTTP response";
}

// Each request owns its socket and TLS state. Destruction never emits network
// traffic (in particular, no potentially blocking SSL_shutdown()).
class SocketConnection {
public:
    explicit SocketConnection(int socket) : fd_(socket) {}
    ~SocketConnection() { ::close(fd_); }
    SocketConnection(const SocketConnection&) = delete;
    SocketConnection& operator=(const SocketConnection&) = delete;

    void deadline(int milliseconds) {
        deadline_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    }
    void check_deadline() {
        if (!deadline_) return;
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(*deadline_ - std::chrono::steady_clock::now()).count();
        if (remaining <= 0) throw Error(ErrorCode::Timeout, "WebSocket operation timed out");
        const timeval timeout{static_cast<long>(remaining / 1000), static_cast<long>((remaining % 1000) * 1000)};
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        ::setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    }

    void enable_tls(const std::string& host) {
#ifdef PROWSETK_HAVE_OPENSSL
        std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> context(SSL_CTX_new(TLS_client_method()), SSL_CTX_free);
        if (!context || SSL_CTX_set_min_proto_version(context.get(), TLS1_2_VERSION) != 1 ||
            SSL_CTX_set_default_verify_paths(context.get()) != 1) {
            throw Error(ErrorCode::NetworkError, "TLS trust store initialization failed");
        }
        SSL_CTX_set_verify(context.get(), SSL_VERIFY_PEER, nullptr);
        std::unique_ptr<SSL, decltype(&SSL_free)> ssl(SSL_new(context.get()), SSL_free);
        if (!ssl || SSL_set_tlsext_host_name(ssl.get(), host.c_str()) != 1) {
            throw Error(ErrorCode::NetworkError, "TLS initialization failed");
        }
        if (ssl_) {
            // HTTPS through an HTTPS proxy is TLS-over-TLS. The inner SSL owns
            // the filter BIO; the outer proxy SSL/context outlive that BIO.
            std::unique_ptr<BIO, decltype(&BIO_free)> tunnel(BIO_new(BIO_f_ssl()), BIO_free);
            if (!tunnel || BIO_set_ssl(tunnel.get(), ssl_.get(), BIO_NOCLOSE) != 1)
                throw Error(ErrorCode::NetworkError, "TLS proxy tunnel initialization failed");
            SSL_set_bio(ssl.get(), tunnel.get(), tunnel.get());
            tunnel.release();
            proxy_context_ = std::move(context_);
            proxy_ssl_ = std::move(ssl_);
        } else if (SSL_set_fd(ssl.get(), fd_) != 1) {
            throw Error(ErrorCode::NetworkError, "TLS initialization failed");
        }
        SSL_set_hostflags(ssl.get(), X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
        // X509_VERIFY_PARAM handles literal IP SANs too, including OpenSSL 3.0.
        auto* parameters = SSL_get0_param(ssl.get());
        if (X509_VERIFY_PARAM_set1_ip_asc(parameters, host.c_str()) != 1 &&
            SSL_set1_host(ssl.get(), host.c_str()) != 1) {
            throw Error(ErrorCode::NetworkError, "TLS hostname configuration failed");
        }
        context_ = std::move(context);
        ssl_ = std::move(ssl);
        ERR_clear_error();
        const int result = SSL_connect(ssl_.get());
        if (result != 1) tls_error(result, "TLS handshake failed");
        if (SSL_get_verify_result(ssl_.get()) != X509_V_OK) {
            throw Error(ErrorCode::NetworkError, "TLS certificate verification failed");
        }
#else
        (void)host;
        throw Error(ErrorCode::Unsupported, "HTTPS requires an OpenSSL-enabled build");
#endif
    }

    ssize_t write(const char* bytes, std::size_t size) {
        check_deadline();
#ifdef PROWSETK_HAVE_OPENSSL
        if (ssl_) {
            std::size_t written = 0;
            ERR_clear_error();
            const int result = SSL_write_ex(ssl_.get(), bytes, size, &written);
            if (result != 1) tls_error(result, "TLS write failed");
            return static_cast<ssize_t>(written);
        }
#endif
#ifdef MSG_NOSIGNAL
        return ::send(fd_, bytes, size, MSG_NOSIGNAL);
#else
        return ::send(fd_, bytes, size, 0);
#endif
    }

    ssize_t read(char* bytes, std::size_t size) {
        check_deadline();
#ifdef PROWSETK_HAVE_OPENSSL
        if (ssl_) {
            std::size_t received = 0;
            ERR_clear_error();
            const int result = SSL_read_ex(ssl_.get(), bytes, size, &received);
            if (result != 1) {
                if (SSL_get_error(ssl_.get(), result) == SSL_ERROR_ZERO_RETURN) return 0;
                tls_error(result, "TLS read failed");
            }
            return static_cast<ssize_t>(received);
        }
#endif
        return ::recv(fd_, bytes, size, 0);
    }

private:
    int fd_;
    std::optional<std::chrono::steady_clock::time_point> deadline_;
#ifdef PROWSETK_HAVE_OPENSSL
    std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> proxy_context_{nullptr, SSL_CTX_free};
    std::unique_ptr<SSL, decltype(&SSL_free)> proxy_ssl_{nullptr, SSL_free};
    std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> context_{nullptr, SSL_CTX_free};
    std::unique_ptr<SSL, decltype(&SSL_free)> ssl_{nullptr, SSL_free};
    [[noreturn]] void tls_error(int result, const char* message) {
        const int code = SSL_get_error(ssl_.get(), result);
        if (code == SSL_ERROR_WANT_READ || code == SSL_ERROR_WANT_WRITE ||
            (code == SSL_ERROR_SYSCALL && (errno == EAGAIN || errno == EWOULDBLOCK))) {
            throw Error(ErrorCode::Timeout, "TLS operation timed out");
        }
        if (SSL_get_verify_result(ssl_.get()) != X509_V_OK) {
            throw Error(ErrorCode::NetworkError, "TLS certificate verification failed");
        }
        throw Error(ErrorCode::NetworkError, message);
    }
#endif
};

#ifdef PROWSETK_HAVE_OPENSSL
std::string websocket_base64(const unsigned char* bytes, std::size_t count) {
    std::string result(4 * ((count + 2) / 3), '\0');
    EVP_EncodeBlock(reinterpret_cast<unsigned char*>(result.data()), bytes, static_cast<int>(count));
    return result;
}
class SocketWebSocket final : public WebSocket {
public:
    SocketWebSocket(std::unique_ptr<SocketConnection> socket, int timeout, std::size_t limit)
        : socket_(std::move(socket)), timeout_(timeout), limit_(limit) {}
    void send(std::string_view text) override {
        socket_->deadline(timeout_);
        if (text.size() > limit_) throw Error(ErrorCode::ResourceLimit, "WebSocket message limit");
        try { flatworm::rpc::validate_utf8(text); }
        catch (...) { throw Error(ErrorCode::ParseError, "invalid WebSocket UTF-8"); }
        frame(1, text);
    }
    std::string receive() override {
        // The last send starts the transaction deadline; notifications must not
        // extend it indefinitely while a CDP response is outstanding.
        std::string message;
        bool started = false;
        for (unsigned frames = 0; frames < 4096; ++frames) {
            const auto head = read(2);
            const auto first = static_cast<unsigned char>(head[0]);
            const auto second = static_cast<unsigned char>(head[1]);
            const auto opcode = first & 15;
            const bool final = (first & 128) != 0;
            if ((first & 112) || (second & 128)) fail();
            std::uint64_t size = second & 127;
            if (size == 126 || size == 127) {
                const auto extended = read(size == 126 ? 2 : 8);
                size = 0;
                for (unsigned char byte : extended) size = (size << 8) | byte;
                if ((extended.size() == 2 && size < 126) || (extended.size() == 8 && (size < 65536 || size >> 63))) fail();
            }
            if (opcode >= 8 && (!final || size > 125)) fail();
            if (size > limit_ || (opcode < 8 && size > limit_ - message.size())) throw Error(ErrorCode::ResourceLimit, "WebSocket message limit");
            auto payload = read(static_cast<std::size_t>(size));
            if (opcode == 8) throw Error(ErrorCode::NetworkError, "WebSocket peer closed");
            if (opcode == 9) { frame(10, payload); continue; }
            if (opcode == 10) continue;
            if ((opcode != 0 && opcode != 1) || (opcode == 0 && !started) || (opcode == 1 && started)) fail();
            started = true;
            message += payload;
            if (final) {
                try { flatworm::rpc::validate_utf8(message); } catch (...) { fail(); }
                return message;
            }
        }
        throw Error(ErrorCode::ResourceLimit, "WebSocket frame limit");
    }
private:
    std::unique_ptr<SocketConnection> socket_;
    int timeout_;
    std::size_t limit_;
    [[noreturn]] static void fail() { throw Error(ErrorCode::ParseError, "invalid WebSocket frame"); }
    std::string read(std::size_t size) {
        std::string result(size, '\0');
        std::size_t done = 0;
        while (done < size) {
            const auto n = socket_->read(result.data() + done, size - done);
            if (n <= 0) throw Error(ErrorCode::NetworkError, "WebSocket read failed");
            done += static_cast<std::size_t>(n);
        }
        return result;
    }
    void frame(unsigned char opcode, std::string_view text) {
        std::array<unsigned char, 4> mask{};
        if (RAND_bytes(mask.data(), 4) != 1) throw Error(ErrorCode::Internal, "WebSocket randomness failed");
        std::string wire(1, static_cast<char>(128 | opcode));
        if (text.size() < 126) wire += static_cast<char>(128 | text.size());
        else {
            const unsigned count = text.size() <= 65535 ? 2 : 8;
            wire += static_cast<char>(128 | (count == 2 ? 126 : 127));
            for (unsigned i = count; i > 0; --i) wire += static_cast<char>((static_cast<std::uint64_t>(text.size()) >> ((i - 1) * 8)) & 255);
        }
        for (auto byte : mask) wire += static_cast<char>(byte);
        for (std::size_t i = 0; i < text.size(); ++i) wire += static_cast<char>(static_cast<unsigned char>(text[i]) ^ mask[i % 4]);
        std::size_t done = 0;
        while (done < wire.size()) {
            const auto n = socket_->write(wire.data() + done, wire.size() - done);
            if (n <= 0) throw Error(ErrorCode::NetworkError, "WebSocket write failed");
            done += static_cast<std::size_t>(n);
        }
    }
};
#endif

class SocketNetworkClient : public NetworkClient {
public:
    std::unique_ptr<WebSocket> open_websocket(const HttpRequest& request) override {
#ifdef PROWSETK_HAVE_OPENSSL
        if (request.timeout_ms < 1 || request.timeout_ms > 300000 || request.max_response_bytes < 1 ||
            request.max_response_bytes > 16 * 1024 * 1024 || request.url.size() > 8192 ||
            request.url.find('@') != std::string::npos || request.url.find('#') != std::string::npos ||
            std::any_of(request.url.begin(), request.url.end(), [](unsigned char c) { return c <= 32 || c == 127; }))
            throw Error(ErrorCode::InvalidArgument, "invalid WebSocket request");
        auto endpoint = request.url;
        if (endpoint.starts_with("wss://")) endpoint.replace(0, 3, "https");
        else if (endpoint.starts_with("ws://")) endpoint.replace(0, 2, "http");
        else throw Error(ErrorCode::InvalidUrl, "WebSocket requires ws or wss");
        const auto url = parse_url(endpoint);
        const auto selected = request.proxy.enabled() ? request.proxy : (proxy_.enabled() ? proxy_ : proxy_from_environment(endpoint));
        const auto port = url.port.empty() ? (url.scheme == "https" ? "443" : "80") : url.port;
        auto socket = std::make_unique<SocketConnection>(open_connection(selected.enabled() ? selected.host : unbracket_host(url.host), selected.enabled() ? selected.port : port, request.timeout_ms));
        socket->deadline(request.timeout_ms);
        if (selected.scheme == ProxyScheme::Https) socket->enable_tls(selected.host);
        if (selected.scheme == ProxyScheme::Socks5) socks5_handshake(*socket, selected, url.host, port);
        else if (selected.enabled()) {
            write_all(*socket, "CONNECT " + url.host + ":" + port + " HTTP/1.1\r\nHost: " + url.host + ":" + port + proxy_authorization(selected) + "\r\n\r\n");
            read_proxy_connect(*socket);
        }
        if (url.scheme == "https") socket->enable_tls(unbracket_host(url.host));
        std::array<unsigned char, 16> nonce{};
        if (RAND_bytes(nonce.data(), 16) != 1) throw Error(ErrorCode::Internal, "WebSocket randomness failed");
        const auto key = websocket_base64(nonce.data(), nonce.size());
        const auto challenge = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        std::array<unsigned char, SHA_DIGEST_LENGTH> hash{};
        SHA1(reinterpret_cast<const unsigned char*>(challenge.data()), challenge.size(), hash.data());
        auto path = url.path.empty() ? "/" : url.path;
        if (url.has_query) path += "?" + url.query;
        std::string wire = "GET " + path + " HTTP/1.1\r\nHost: " + url.host + ":" + port +
            "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: " + key;
        std::size_t header_bytes = 0;
        for (const auto& [name, value] : request.headers) {
            header_bytes += name.size() + value.size();
            const auto lower = to_lower(name);
            if (name.empty() || header_bytes > 32768 || !std::all_of(name.begin(), name.end(), [](unsigned char c) { return std::isalnum(c) || c == '-'; }) ||
                std::any_of(value.begin(), value.end(), [](unsigned char c) { return c < 32 || c == 127; }) ||
                lower == "host" || lower == "connection" || lower == "upgrade" || lower.starts_with("sec-websocket") ||
                lower == "proxy-authorization" || lower == "content-length" || lower == "transfer-encoding")
                throw Error(ErrorCode::InvalidArgument, "invalid WebSocket header");
            wire += "\r\n" + name + ": " + value;
        }
        write_all(*socket, wire + "\r\n\r\n");
        std::string response;
        while (!response.ends_with("\r\n\r\n")) {
            char byte;
            if (response.size() >= 65536) throw Error(ErrorCode::ResourceLimit, "WebSocket handshake limit");
            if (socket->read(&byte, 1) != 1) throw Error(ErrorCode::NetworkError, "WebSocket handshake failed");
            response += byte;
        }
        if (!response.starts_with("HTTP/1.1 101 ")) throw Error(ErrorCode::NetworkError, "WebSocket upgrade rejected");
        std::map<std::string, std::string> headers;
        std::istringstream lines(response.substr(response.find("\r\n") + 2));
        std::string line;
        while (std::getline(lines, line) && line != "\r") {
            const auto colon = line.find(':');
            if (colon == std::string::npos || colon == 0 || line.front() == ' ' || line.front() == '\t')
                throw Error(ErrorCode::NetworkError, "invalid WebSocket handshake");
            const auto name = to_lower(line.substr(0, colon));
            const auto value = trim(line.substr(colon + 1));
            if (name == "connection" || name == "upgrade") {
                if (headers.contains(name)) headers[name] += "," + value;
                else headers[name] = value;
            } else if (name.starts_with("sec-websocket-")) {
                if (!headers.emplace(name, value).second) throw Error(ErrorCode::NetworkError, "invalid WebSocket handshake");
            }
        }
        const auto has_token = [](const std::string& list, std::string_view wanted) {
            std::istringstream tokens(to_lower(list));
            std::string token;
            while (std::getline(tokens, token, ',')) if (trim(token) == wanted) return true;
            return false;
        };
        if (!has_token(headers["upgrade"], "websocket") || !has_token(headers["connection"], "upgrade") ||
            headers["sec-websocket-accept"] != websocket_base64(hash.data(), hash.size()) ||
            headers.contains("sec-websocket-extensions") || headers.contains("sec-websocket-protocol"))
            throw Error(ErrorCode::NetworkError, "invalid WebSocket handshake");
        return std::make_unique<SocketWebSocket>(std::move(socket), request.timeout_ms, request.max_response_bytes);
#else
        return NetworkClient::open_websocket(request);
#endif
    }
    HttpResponse send(const HttpRequest& request) override {
        const Url url = parse_url(request.url);
        ProxyConfig selected = request.proxy.enabled() ? request.proxy : (proxy_.enabled() ? proxy_ : proxy_from_environment(request.url));
        if (url.scheme != "http" && url.scheme != "https") {
            throw Error(ErrorCode::Unsupported,
                        "socket client supports HTTP and HTTPS only");
        }
        #ifndef PROWSETK_HAVE_OPENSSL
        if (url.scheme == "https") {
            throw Error(ErrorCode::Unsupported, "HTTPS requires an OpenSSL-enabled build");
        }
#endif
        const std::string port = url.port.empty()
            ? (url.scheme == "https" ? "443" : "80") : url.port;

        const std::string connect_host = selected.enabled() ? selected.host : unbracket_host(url.host);
        const std::string connect_port = selected.enabled() ? selected.port : port;
        SocketConnection connection(open_connection(connect_host, connect_port, request.timeout_ms));
        if (selected.scheme == ProxyScheme::Https) connection.enable_tls(selected.host);
        if (selected.scheme == ProxyScheme::Socks5) socks5_handshake(connection, selected, url.host, port);
        else if (selected.enabled() && url.scheme == "https") {
            std::string connect = "CONNECT " + url.host + ":" + port + " HTTP/1.1\r\nHost: " + url.host + ":" + port;
            connect += proxy_authorization(selected);
            connect += "\r\n\r\n";
            write_all(connection, connect);
            read_proxy_connect(connection);
        }
        if (url.scheme == "https") connection.enable_tls(unbracket_host(url.host));

        std::string path = url.path.empty() ? "/" : url.path;
        if (url.has_query) {
            path += "?" + url.query;
        }
        const bool forward_proxy = selected.enabled() && selected.scheme != ProxyScheme::Socks5 && url.scheme == "http";
        if (forward_proxy) path = url.origin() + path;
        std::string payload =
            request.method + " " + path + " HTTP/1.1\r\nHost: " + url.host;
        if (!url.port.empty()) payload += ":" + url.port;
        bool has_user_agent = false;
        for (const auto& [name, value] : request.headers) {
            // Proxy credentials belong only to the proxy hop, never the
            // tunneled origin (even if a caller supplied this header).
            if (to_lower(name) == "proxy-authorization") continue;
            payload += "\r\n" + name + ": " + value;
            if (to_lower(name) == "user-agent") {
                has_user_agent = true;
            }
        }
        if (!has_user_agent) {
            payload += "\r\nUser-Agent: ProwseTk/0.1";
        }
        if (forward_proxy) payload += proxy_authorization(selected);
        payload += "\r\nConnection: close\r\n";
        if (!request.body.empty()) {
            payload += "Content-Length: " +
                       std::to_string(request.body.size()) + "\r\n";
        }
        payload += "\r\n" + request.body;

        std::size_t sent = 0;
        while (sent < payload.size()) {
            const ssize_t n = connection.write(payload.data() + sent, payload.size() - sent);
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                throw Error(ErrorCode::Timeout,
                            "request timed out while sending to " + url.host);
            }
            if (n <= 0) {
                throw Error(ErrorCode::NetworkError, "socket send failed");
            }
            sent += static_cast<std::size_t>(n);
        }

        HttpResponse response = read_response(connection, request.max_response_bytes);
        response.final_url = request.url;
        return response;
    }

    std::string name() const override { return "socket"; }
    void set_proxy(ProxyConfig proxy) override { proxy_ = std::move(proxy); }
    ProxyConfig proxy() const override { return proxy_; }

private:
    ProxyConfig proxy_;
    static void write_all(SocketConnection& connection, const std::string& payload) {
        std::size_t sent = 0; while (sent < payload.size()) { const auto n = connection.write(payload.data()+sent, payload.size()-sent); if (n <= 0) throw Error(ErrorCode::NetworkError, "proxy write failed"); sent += static_cast<std::size_t>(n); }
    }
    static void read_proxy_connect(SocketConnection& connection) {
        std::string raw;
        while (!raw.ends_with("\r\n\r\n")) {
            if (raw.size() >= 65536) throw Error(ErrorCode::ResourceLimit, "proxy response too large");
            char byte;
            const auto n = connection.read(&byte, 1);
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                throw Error(ErrorCode::Timeout, "proxy CONNECT timed out");
            if (n <= 0) throw Error(ErrorCode::NetworkError, "proxy CONNECT failed");
            raw += byte;
        }
        const auto end = raw.find("\r\n");
        const auto line = std::string_view(raw).substr(0, end);
        int status = 0;
        if (line.size() < 12 || (!line.starts_with("HTTP/1.1 ") && !line.starts_with("HTTP/1.0 ")) ||
            (line.size() > 12 && line[12] != ' ')) throw Error(ErrorCode::NetworkError, "malformed proxy CONNECT response");
        const auto parsed = std::from_chars(line.data() + 9, line.data() + 12, status);
        if (parsed.ec != std::errc{} || parsed.ptr != line.data() + 12)
            throw Error(ErrorCode::NetworkError, "malformed proxy CONNECT response");
        if (status < 200 || status >= 300)
            throw Error(ErrorCode::NetworkError, "proxy CONNECT rejected (HTTP " + std::to_string(status) + ")");
    }
    static void socks5_handshake(SocketConnection& connection, const ProxyConfig& proxy, const std::string& host, const std::string& port) {
        std::string hello; hello.push_back(5); hello.push_back(proxy.username.empty()?1:2); hello.push_back(0); if(!proxy.username.empty()) hello.push_back(2); write_all(connection,hello); char reply[2]; if(connection.read(reply,2)!=2 || reply[0]!=5 || static_cast<unsigned char>(reply[1])==255) throw Error(ErrorCode::NetworkError,"SOCKS5 authentication rejected");
        if(reply[1]==2) { std::string auth; auth.push_back(1); auth.push_back(static_cast<char>(proxy.username.size())); auth+=proxy.username; auth.push_back(static_cast<char>(proxy.password.size())); auth+=proxy.password; write_all(connection,auth); char ar[2]; if(connection.read(ar,2)!=2 || ar[1]!=0) throw Error(ErrorCode::NetworkError,"SOCKS5 authentication failed"); }
        std::string req{char(5),char(1),char(0),char(3),char(host.size())}; req+=host; auto p=std::stoi(port); req.push_back(static_cast<char>((p>>8)&255)); req.push_back(static_cast<char>(p&255)); write_all(connection,req); char head[4]; if(connection.read(head,4)!=4 || head[1]!=0) throw Error(ErrorCode::NetworkError,"SOCKS5 CONNECT failed"); std::size_t skip=head[3]==1?4:head[3]==4?16:0; if(head[3]==3){char n; if(connection.read(&n,1)!=1) throw Error(ErrorCode::NetworkError,"SOCKS5 response truncated"); skip=static_cast<unsigned char>(n);} char sink[256]; while(skip){auto n=connection.read(sink,std::min(skip,sizeof(sink))); if(n<=0) throw Error(ErrorCode::NetworkError,"SOCKS5 response truncated"); skip-=static_cast<std::size_t>(n);}
    }
    int open_connection(const std::string& host, const std::string& port,
                        int timeout_ms) {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* result = nullptr;
        const int rc = ::getaddrinfo(host.c_str(), port.c_str(), &hints,
                                     &result);
        if (rc != 0 || result == nullptr) {
            throw Error(ErrorCode::NetworkError,
                        "DNS resolution failed for " + host);
        }

        int fd = -1;
        int last_error = ECONNREFUSED;
        const bool bounded = timeout_ms > 0;
        for (addrinfo* candidate = result; candidate != nullptr;
             candidate = candidate->ai_next) {
            fd = ::socket(candidate->ai_family, candidate->ai_socktype,
                          candidate->ai_protocol);
            if (fd < 0) {
                continue;
            }
            const int flags = ::fcntl(fd, F_GETFL, 0);
            ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
            if (::connect(fd, candidate->ai_addr, candidate->ai_addrlen) == 0) {
                ::fcntl(fd, F_SETFL, flags);
                break;
            }
            if (errno == EINPROGRESS && bounded) {
                pollfd poll_fd{fd, POLLOUT, 0};
                const int poll_rc =
                    ::poll(&poll_fd, 1, timeout_ms);
                if (poll_rc > 0 && (poll_fd.revents & POLLOUT)) {
                    int socket_error = 0;
                    socklen_t error_len = sizeof(socket_error);
                    ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error,
                                 &error_len);
                    if (socket_error == 0) {
                        ::fcntl(fd, F_SETFL, flags);
                        break;
                    }
                    last_error = socket_error;
                } else if (poll_rc == 0) {
                    last_error = ETIMEDOUT;
                }
            } else {
                last_error = errno;
            }
            ::close(fd);
            fd = -1;
        }
        ::freeaddrinfo(result);
        if (fd < 0) {
            if (last_error == ETIMEDOUT) {
                throw Error(ErrorCode::Timeout,
                            "connection to " + host + ":" + port +
                                " timed out");
            }
            throw Error(ErrorCode::NetworkError,
                        "connection failed for " + host + ":" + port);
        }

        if (bounded) {
            const timeval timeout{timeout_ms / 1000,
                                  (timeout_ms % 1000) * 1000};
            ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                         sizeof(timeout));
            ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                         sizeof(timeout));
        }
        return fd;
    }

    static bool recv_more(SocketConnection& connection, std::string& raw) {
        char buffer[8192];
        const ssize_t n = connection.read(buffer, sizeof(buffer));
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            throw Error(ErrorCode::Timeout,
                        "response read timed out before completion");
        }
        if (n <= 0) {
            return false;
        }
        raw.append(buffer, static_cast<std::size_t>(n));
        return true;
    }

    // Reads until `needle` appears at or after `offset` in `raw`, appending
    // socket data as required. Returns false on EOF before a match.
    static bool fill_until(SocketConnection& connection, std::string& raw, std::size_t offset,
                           const std::string& needle) {
        while (true) {
            const std::size_t found = raw.find(needle, offset);
            if (found != std::string::npos) {
                return true;
            }
            if (!recv_more(connection, raw)) {
                return false;
            }
        }
    }

    // Decodes a chunked-transfer body beginning at `raw[offset]`. Appends to
    // `raw` as needed and returns the decoded body.
    static std::string decode_chunked(SocketConnection& connection, std::string& raw,
                                      std::size_t offset,
                                      std::size_t max_bytes) {
        std::string body;
        while (true) {
            if (!fill_until(connection, raw, offset, "\r\n")) {
                throw Error(ErrorCode::NetworkError,
                            "truncated chunked response");
            }
            const std::size_t line_end = raw.find("\r\n", offset);
            std::string line = raw.substr(offset, line_end - offset);
            offset = line_end + 2;

            const auto semicolon = line.find(';');
            if (semicolon != std::string::npos) {
                line.resize(semicolon);
            }
            const auto trim_hex = [](std::string value) {
                const auto not_space = [](unsigned char c) {
                    return std::isspace(c) == 0;
                };
                value.erase(value.begin(),
                            std::find_if(value.begin(), value.end(),
                                         not_space));
                value.erase(std::find_if(value.rbegin(), value.rend(),
                                         not_space)
                                .base(),
                            value.end());
                return value;
            };
            line = trim_hex(std::move(line));
            if (line.empty()) {
                throw Error(ErrorCode::NetworkError,
                            "invalid chunked size line");
            }
            std::size_t chunk_size = 0;
            const char* begin = line.c_str();
            char* end = nullptr;
            const unsigned long parsed =
                std::strtoul(begin, &end, 16);
            if (end == begin) {
                throw Error(ErrorCode::NetworkError,
                            "invalid chunked size");
            }
            chunk_size = static_cast<std::size_t>(parsed);

            if (chunk_size == 0) {
                if (!fill_until(connection, raw, offset, "\r\n")) {
                    throw Error(ErrorCode::NetworkError,
                                "truncated chunked trailer");
                }
                return body;
            }
            if (body.size() + chunk_size > max_bytes) {
                throw Error(ErrorCode::ResourceLimit,
                            "response body exceeds the configured limit");
            }
            if (!fill_until(connection, raw, offset + chunk_size, "\r\n")) {
                throw Error(ErrorCode::NetworkError,
                            "truncated chunk data");
            }
            body.append(raw, offset, chunk_size);
            offset += chunk_size + 2;
        }
    }

    static HttpResponse read_response(SocketConnection& connection, std::size_t max_bytes) {
        std::string raw;
        if (!fill_until(connection, raw, 0, "\r\n\r\n")) {
            throw Error(ErrorCode::NetworkError, read_status_line_error());
        }
        const std::size_t header_end = raw.find("\r\n\r\n");
        std::string head = raw.substr(0, header_end);

        HttpResponse response;
        const auto line_end = head.find("\r\n");
        const std::string status_line = head.substr(0, line_end);
        const auto first_space = status_line.find(' ');
        if (first_space != std::string::npos) {
            response.status = std::atoi(status_line.c_str() + first_space + 1);
        }
        std::size_t cursor =
            line_end == std::string::npos ? head.size() : line_end + 2;
        while (cursor < head.size()) {
            const auto end = head.find("\r\n", cursor);
            const std::string line =
                head.substr(cursor, end == std::string::npos
                                        ? std::string::npos
                                        : end - cursor);
            const auto colon = line.find(':');
            if (colon != std::string::npos) {
                response.headers.emplace_back(
                    trim(line.substr(0, colon)),
                    trim(line.substr(colon + 1)));
            }
            if (end == std::string::npos) {
                break;
            }
            cursor = end + 2;
        }

        std::size_t body_offset = header_end + 4;
        const std::string transfer_encoding =
            to_lower(response.header("Transfer-Encoding"));
        if (transfer_encoding.find("chunked") != std::string::npos) {
            response.body =
                decode_chunked(connection, raw, body_offset, max_bytes);
            return response;
        }

        const std::string content_length = response.header("Content-Length");
        if (!content_length.empty()) {
            const unsigned long length =
                std::strtoul(content_length.c_str(), nullptr, 10);
            if (length > max_bytes) {
                throw Error(ErrorCode::ResourceLimit,
                            "response body exceeds the configured limit");
            }
            while (raw.size() < body_offset + length) {
                if (!recv_more(connection, raw)) {
                    throw Error(ErrorCode::NetworkError, "truncated HTTP response body");
                }
            }
            response.body = raw.substr(body_offset, length);
            return response;
        }

        while (raw.size() - body_offset < max_bytes) {
            if (!recv_more(connection, raw)) {
                break;
            }
        }
        if (raw.size() - body_offset > max_bytes) {
            throw Error(ErrorCode::ResourceLimit,
                        "response body exceeds the configured limit");
        }
        response.body = raw.substr(body_offset);
        return response;
    }
};
}  // namespace
#endif

std::unique_ptr<NetworkClient> make_socket_network_client() {
#if defined(__unix__) || defined(__APPLE__)
    return std::make_unique<SocketNetworkClient>();
#else
    throw Error(ErrorCode::Unsupported,
                "no socket network client on this platform");
#endif
}

}  // namespace prowsetk
