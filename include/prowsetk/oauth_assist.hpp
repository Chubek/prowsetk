#ifndef PROWSETK_OAUTH_ASSIST_HPP
#define PROWSETK_OAUTH_ASSIST_HPP
#include <chrono>
#include <filesystem>
#include <string>
#include "prowsetk/network_client.hpp"

namespace prowsetk::oauth_assist {
struct Config {
    std::string authorization_endpoint = "https://dash.cloudflare.com/oauth2/auth";
    std::string token_endpoint = "https://dash.cloudflare.com/oauth2/token";
    std::string client_id;
    std::string redirect_uri = "http://localhost:8976/oauth/callback";
    std::string scopes;
};
struct Token {
    std::string access_token;
    std::string refresh_token;
    std::int64_t expires_at = 0;
};
// One-use authorization transaction. Pass the complete redirect URL to finish.
// Open authorization_url() in a user-controlled browser; no password collection.
class Authorization {
public:
    explicit Authorization(Config config);
    Authorization(const Authorization&) = delete;
    Authorization& operator=(const Authorization&) = delete;
    std::string authorization_url() const;
    Token finish(NetworkClient& network, std::string_view redirect_url);
private:
    Config config_;
    std::string verifier_, state_, challenge_;
    std::chrono::steady_clock::time_point deadline_;
    bool consumed_ = false;
};
Token refresh(NetworkClient& network, const Config& config, const Token& token);
std::filesystem::path default_cache_directory();
// Cache records are bound to issuer, client, redirect and scopes. POSIX only;
// owner-only directories/files, no symlink traversal, atomic replacement.
void save(const std::filesystem::path& directory, const Config& config, const Token& token);
Token load(const std::filesystem::path& directory, const Config& config);
void logout(const std::filesystem::path& directory, const Config& config);
bool expired(const Token& token);
}
#endif
