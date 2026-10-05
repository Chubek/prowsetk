#include "prowsetk/oauth_assist.hpp"
#include "prowsetk/url.hpp"
#include "protocol_json.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#ifdef PROWSETK_HAVE_OPENSSL
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/evp.h>
#endif
#ifdef PROWSETK_HAVE_OAUTHCPP
#include <liboauthcpp/liboauthcpp.h>
#endif

namespace prowsetk::oauth_assist {
namespace {
using namespace protocol;
std::int64_t now() { return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
void checked_text(std::string_view text, bool allow_empty = false) {
    if ((!allow_empty && text.empty()) || text.size() > 16384 ||
        std::any_of(text.begin(), text.end(), [](unsigned char c) { return c < 32 || c == 127; }))
        throw Error(ErrorCode::InvalidArgument, "invalid OAuth field");
}
std::string escape(const std::string& text) {
#ifdef PROWSETK_HAVE_OAUTHCPP
    return OAuth::PercentEncode(text);
#else
    std::string result;
    constexpr char hex[] = "0123456789ABCDEF";
    for (unsigned char c : text) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~') result += static_cast<char>(c);
        else { result += '%'; result += hex[c >> 4]; result += hex[c & 15]; }
    }
    return result;
#endif
}
void validate(const Config& config) {
    checked_text(config.client_id);
    checked_text(config.scopes);
    for (const auto& endpoint : {config.authorization_endpoint, config.token_endpoint}) {
        checked_text(endpoint);
        const auto url = parse_url(endpoint);
        if (url.scheme != "https" || url.host.empty() || endpoint.find_first_of("@?# ") != std::string::npos)
            throw Error(ErrorCode::SecurityViolation, "OAuth requires HTTPS endpoints");
    }
    checked_text(config.redirect_uri);
    const auto redirect = parse_url(config.redirect_uri);
    if (config.redirect_uri.find_first_of("@?# ") != std::string::npos || redirect.host.empty() ||
        (redirect.scheme != "https" && !(redirect.scheme == "http" && (redirect.host == "localhost" || redirect.host == "127.0.0.1"))))
        throw Error(ErrorCode::SecurityViolation, "invalid OAuth redirect URI");
}
#ifdef PROWSETK_HAVE_OPENSSL
std::string base64url(const unsigned char* data, int length) {
    std::string result(static_cast<std::size_t>(4 * ((length + 2) / 3)), '\0');
    EVP_EncodeBlock(reinterpret_cast<unsigned char*>(result.data()), data, length);
    while (result.ends_with('=')) result.pop_back();
    std::replace(result.begin(), result.end(), '+', '-');
    std::replace(result.begin(), result.end(), '/', '_');
    return result;
}
std::string random() {
    std::array<unsigned char, 32> bytes{};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) throw Error(ErrorCode::Internal, "OAuth randomness failed");
    return base64url(bytes.data(), static_cast<int>(bytes.size()));
}
#endif
std::string decode(std::string_view text) {
    std::string result;
    const auto hex = [](char c) -> int { if (c >= '0' && c <= '9') return c - '0'; if (c >= 'a' && c <= 'f') return c - 'a' + 10; if (c >= 'A' && c <= 'F') return c - 'A' + 10; return -1; };
    for (std::size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (c == '%') {
            if (i + 2 >= text.size() || hex(text[i + 1]) < 0 || hex(text[i + 2]) < 0) throw Error(ErrorCode::ParseError, "invalid OAuth redirect");
            c = static_cast<char>((hex(text[i + 1]) << 4) | hex(text[i + 2])); i += 2;
        } else if (c == '+') c = ' ';
        result += c;
    }
    checked_text(result);
    return result;
}
Token exchange(NetworkClient& network, const Config& config, const std::string& body) {
    validate(config);
    HttpRequest request;
    request.method = "POST"; request.url = config.token_endpoint;
    request.body = body + "&client_id=" + escape(config.client_id);
    request.headers = {{"Content-Type", "application/x-www-form-urlencoded"}, {"Accept", "application/json"}};
    request.max_response_bytes = 65536;
    HttpResponse response;
    try { response = network.send(request); }
    catch (...) { throw Error(ErrorCode::NetworkError, "OAuth exchange failed"); }
    if (response.status != 200 || !response.redirect_chain.empty() || (!response.final_url.empty() && response.final_url != request.url) || response.body.size() > 65536)
        throw Error(ErrorCode::NetworkError, "OAuth exchange rejected");
    const auto json = parse(response.body);
    if (string(json, "token_type") != "bearer" && string(json, "token_type") != "Bearer") throw Error(ErrorCode::ParseError, "unsupported OAuth token type");
    Token token;
    token.access_token = string(json, "access_token"); checked_text(token.access_token);
    if (json.find("refresh_token")) { token.refresh_token = string(json, "refresh_token"); checked_text(token.refresh_token); }
    const auto* expires = json.find("expires_in");
    if (!expires || expires->kind != Json::Kind::Number || expires->number < 1 || expires->number > 31536000 || std::floor(expires->number) != expires->number)
        throw Error(ErrorCode::ParseError, "invalid OAuth expiry");
    token.expires_at = now() + static_cast<std::int64_t>(expires->number);
    return token;
}
}
Authorization::Authorization(Config config) : config_(std::move(config)), deadline_(std::chrono::steady_clock::now() + std::chrono::minutes(5)) {
    validate(config_);
#ifdef PROWSETK_HAVE_OPENSSL
    verifier_ = random(); state_ = random();
    std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
    SHA256(reinterpret_cast<const unsigned char*>(verifier_.data()), verifier_.size(), digest.data());
    challenge_ = base64url(digest.data(), static_cast<int>(digest.size()));
#else
    throw Error(ErrorCode::Unsupported, "OAuth PKCE requires OpenSSL");
#endif
}
std::string Authorization::authorization_url() const {
    return config_.authorization_endpoint + "?response_type=code&client_id=" + escape(config_.client_id) +
        "&redirect_uri=" + escape(config_.redirect_uri) + "&scope=" + escape(config_.scopes) +
        "&state=" + state_ + "&code_challenge=" + challenge_ + "&code_challenge_method=S256";
}
Token Authorization::finish(NetworkClient& network, std::string_view redirect_url) {
    if (consumed_ || std::chrono::steady_clock::now() >= deadline_) throw Error(ErrorCode::Timeout, "OAuth transaction expired or consumed");
    consumed_ = true;
    if (redirect_url.size() > 32768 || !redirect_url.starts_with(config_.redirect_uri + "?") || redirect_url.find('#') != std::string_view::npos)
        throw Error(ErrorCode::SecurityViolation, "OAuth redirect mismatch");
    auto query = redirect_url.substr(config_.redirect_uri.size() + 1);
    std::map<std::string, std::string> values;
    while (!query.empty()) {
        const auto end = query.find('&');
        const auto pair = query.substr(0, end);
        const auto eq = pair.find('=');
        if (eq == std::string_view::npos || !values.emplace(decode(pair.substr(0, eq)), decode(pair.substr(eq + 1))).second)
            throw Error(ErrorCode::ParseError, "invalid OAuth redirect");
        if (end == std::string_view::npos) break;
        query.remove_prefix(end + 1);
    }
    if (values["state"] != state_ || values.contains("error") || values["code"].empty()) throw Error(ErrorCode::SecurityViolation, "OAuth authorization rejected");
    return exchange(network, config_, "grant_type=authorization_code&code=" + escape(values["code"]) + "&redirect_uri=" + escape(config_.redirect_uri) + "&code_verifier=" + escape(verifier_));
}
Token refresh(NetworkClient& network, const Config& config, const Token& token) {
    checked_text(token.refresh_token);
    auto result = exchange(network, config, "grant_type=refresh_token&refresh_token=" + escape(token.refresh_token));
    if (result.refresh_token.empty()) result.refresh_token = token.refresh_token;
    return result;
}
bool expired(const Token& token) { return token.expires_at <= now() + 30; }
}
