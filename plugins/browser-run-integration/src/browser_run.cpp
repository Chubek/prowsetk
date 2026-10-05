#include "prowsetk/browser_run.hpp"
#include "prowsetk/url.hpp"
#include "protocol_json.hpp"
#include <algorithm>
#include <cstdlib>

namespace prowsetk::browser_run {
using namespace protocol;
namespace {
void identifier(std::string_view value) {
    if (value.empty() || value.size() > 256 || !std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    })) throw Error(ErrorCode::InvalidArgument, "invalid Browser Run identifier");
}
void page_url(std::string_view value) {
    if (value.size() > 8192 || value.find('@') != std::string_view::npos ||
        std::any_of(value.begin(), value.end(), [](unsigned char c) { return c <= 32 || c == 127; }))
        throw Error(ErrorCode::InvalidUrl, "invalid remote page URL");
    const auto url = parse_url(value);
    if ((url.scheme != "https" && url.scheme != "http") || url.host.empty()) throw Error(ErrorCode::InvalidUrl, "remote page requires HTTP(S)");
}
void environment(std::string& value, const char* name) { if (const auto* v = std::getenv(name); v && *v) value = v; }
}
Config resolve_config(const ProjectConfig& project, NetworkClient& network) {
    Config result;
    result.account_id = project.cloudflare_account_id; result.api_token = project.cloudflare_api_token;
    environment(result.account_id, "CLOUDFLARE_ACCOUNT_ID"); environment(result.api_token, "CLOUDFLARE_API_TOKEN");
    if (result.api_token.empty()) {
        auto oauth = project.oauth;
        environment(oauth.client_id, "PROWSETK_OAUTH_CLIENT_ID"); environment(oauth.scopes, "PROWSETK_OAUTH_SCOPES");
        if (oauth.token_endpoint != "https://dash.cloudflare.com/oauth2/token") throw Error(ErrorCode::SecurityViolation, "Browser Run requires Cloudflare OAuth issuer");
        const auto cache = oauth_assist::default_cache_directory();
        auto token = oauth_assist::load(cache, oauth);
        if (oauth_assist::expired(token)) { token = oauth_assist::refresh(network, oauth, token); oauth_assist::save(cache, oauth, token); }
        result.api_token = std::move(token.access_token);
    }
    return result;
}
Client::Client(NetworkClient& network, Config config) : network_(network), config_(std::move(config)) {
    if (config_.account_id.size() != 32 || !std::all_of(config_.account_id.begin(), config_.account_id.end(), [](unsigned char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }) ||
        config_.api_token.empty() || config_.api_token.size() > 16384 ||
        std::any_of(config_.api_token.begin(), config_.api_token.end(), [](unsigned char c) { return c <= 32 || c >= 127; }) ||
        config_.timeout_ms < 1 || config_.timeout_ms > 300000 || config_.keep_alive_ms < 10000 || config_.keep_alive_ms > 1200000)
        throw Error(ErrorCode::InvalidArgument, "invalid Browser Run configuration");
    base_ = "https://api.cloudflare.com/client/v4/accounts/" + config_.account_id + "/browser-run";
}
std::unique_ptr<CdpClient> Client::connect(std::string_view browser_session) {
    HttpRequest request;
    request.url = "wss" + base_.substr(5) + "/devtools/browser";
    if (!browser_session.empty()) { identifier(browser_session); request.url += "/" + std::string(browser_session); }
    else request.url += "?keep_alive=" + std::to_string(config_.keep_alive_ms);
    request.headers = {{"Authorization", "Bearer " + config_.api_token}};
    request.timeout_ms = config_.timeout_ms; request.max_response_bytes = 1024 * 1024;
    try { return std::make_unique<CdpClient>(network_.open_websocket(request)); }
    catch (...) { throw Error(ErrorCode::NetworkError, "Browser Run connection failed"); }
}
std::string Client::content(std::string_view url) {
    page_url(url);
    auto body = Json::object_value(); put(body, "url", url);
    HttpRequest request;
    request.method = "POST"; request.url = base_ + "/content"; request.body = encode(body);
    request.headers = {{"Authorization", "Bearer " + config_.api_token}, {"Content-Type", "application/json"}};
    request.timeout_ms = config_.timeout_ms; request.max_response_bytes = 1024 * 1024;
    HttpResponse response;
    try { response = network_.send(request); } catch (...) { throw Error(ErrorCode::NetworkError, "Browser Run request failed"); }
    if (!response.ok() || !response.redirect_chain.empty() || (!response.final_url.empty() && response.final_url != request.url) || response.body.size() > request.max_response_bytes)
        throw Error(ErrorCode::NetworkError, "Browser Run request rejected");
    const auto json = parse(response.body);
    const auto* success = json.find("success");
    if (!success || success->kind != Json::Kind::Boolean || !success->boolean) throw Error(ErrorCode::NetworkError, "Browser Run API failure");
    return string(json, "result");
}
std::string create_page(CdpClient& client, std::string_view url) {
    page_url(url);
    auto args = Json::object_value(); put(args, "url", url);
    return string(parse(client.call("Target.createTarget", encode(args))), "targetId");
}
std::string attach_page(CdpClient& client, std::string_view target_id) {
    identifier(target_id);
    auto args = Json::object_value(); put(args, "targetId", target_id); args.put("flatten", Json::boolean_value(true));
    return string(parse(client.call("Target.attachToTarget", encode(args))), "sessionId");
}
std::string page_html(CdpClient& client, std::string_view session_id) {
    identifier(session_id);
    const auto json = parse(client.call("Runtime.evaluate", R"({"expression":"document.documentElement.outerHTML","returnByValue":true})", session_id));
    if (json.find("exceptionDetails")) throw Error(ErrorCode::JavaScriptError, "remote snapshot evaluation failed");
    const auto* result = json.find("result");
    if (!result || string(*result, "type") != "string") throw Error(ErrorCode::ParseError, "invalid remote snapshot");
    return string(*result, "value");
}
void import_html(Session& session, std::string_view html, std::string_view base_url) {
    if (session.browser().config().javascript) throw Error(ErrorCode::SecurityViolation, "remote snapshot requires JavaScript-disabled session");
    page_url(base_url);
    session.load_html(html, base_url);
}
}
