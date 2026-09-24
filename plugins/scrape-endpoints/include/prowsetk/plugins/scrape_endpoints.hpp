#ifndef PROWSETK_PLUGINS_SCRAPE_ENDPOINTS_HPP
#define PROWSETK_PLUGINS_SCRAPE_ENDPOINTS_HPP

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "prowsetk/browser.hpp"
#include "prowsetk/document.hpp"
#include "prowsetk/endpoint_extraction.hpp"
#include "prowsetk/redaction.hpp"

namespace prowsetk::plugins::scrape_endpoints {

// Default garbage/gunk patterns: static-asset suffixes and asset-directory
// markers for endpoints that cannot serve as proper API endpoints. Shared by
// ScrapeEndpointsOptions and the is_garbage_path empty-pattern fallback
// (mirroring how is_api_path falls back to builtin markers).
const std::vector<std::string>& default_garbage_patterns();

// Unified options for endpoint scraping and export. The OpenAPI and Postman
// exporters share the same host-mediated discovery and SPA probing path.
struct ScrapeEndpointsOptions {
    std::string url;
    std::string html;
    std::string base_url;

    bool follow_links = true;
    bool inspect_scripts = true;
    bool observe_network = true;
    bool infer_schemas = true;
    bool include_provenance = true;
    bool redact_secrets = true;
    std::uint32_t max_depth = 2;
    std::uint32_t max_pages = 100;
    double minimum_confidence = 0.50;
    std::string openapi_version = "3.1.0";

    std::vector<std::string> api_patterns = {"/api", "/v1", "/v2", "/v3",
                                             "/graphql", "/rest", "/internal",
                                             "/data", "/hotel/hoteladmin",
                                             "/partner-settings",
                                             "/telemetry", "challenge",
                                             "/beacon", "/collect"};
    bool require_api_pattern = true;

    // Garbage/gunk filtering ("api-only"): drops endpoints that cannot serve
    // as proper API endpoints — static assets, bundles, images, fonts, media
    // — even when their path carries an API marker or their method is not
    // GET. Applied by filter_to_api when `api_only` is set (the default);
    // set `api_only = false` to keep every discovered endpoint.
    bool api_only = true;
    std::vector<std::string> garbage_patterns = default_garbage_patterns();

    bool resolve_chain = false;
    bool follow_json_links = true;
    std::uint32_t max_resolve_requests = 64;

    // Enables bounded synthetic interaction before extraction. Buttons, submit
    // controls, role=button elements, and javascript/hash links are clicked via
    // Session::click_element, so Flatworm dispatches pointer/focus/click/submit
    // cascades and drains microtasks/network jobs before scraping.
    bool spa_probe = true;
    std::uint32_t max_spa_actions = 32;

    bool assistant_browser_enabled = false;
    bool assistant_prompt = true;
    std::string assistant_browser;
    std::string assistant_browser_method = "webdriver";
    std::string assistant_browser_endpoint;
    std::uint32_t assistant_browser_debug_port = 0;
    std::uint32_t assistant_wait_timeout_ms = 300000;

    std::string success_beacon;
    std::string success_beacon_type = "auto";

    std::string collection_name = "Discovered API (scrape-endpoints)";
};

struct AssistantBrowserHandoff {
    bool needed = false;
    std::string reason;
    std::string url;
    std::string command;
    std::string method;
    std::string endpoint;
    std::uint32_t debug_port = 0;
};

struct ResolvedEndpoint {
    DiscoveredEndpoint endpoint;
    std::string final_url;
    int status = 0;
    std::string body;
    std::string content_type;
    std::vector<std::string> redirect_chain;
    std::vector<std::string> json_discovered;
    std::string error;
};

struct ScrapeEndpointsResult {
    std::vector<ResolvedEndpoint> resolved;
    std::vector<DiscoveredEndpoint> endpoints;
    std::string openapi_yaml;
    std::string postman_json;
    std::vector<std::string> warnings;
    std::optional<AssistantBrowserHandoff> assistant_browser;

    void write_openapi_yaml(const std::filesystem::path& path) const;
    void write_postman_json(const std::filesystem::path& path) const;
};

using Scrape2OapiOptions = ScrapeEndpointsOptions;
using Scrape2OapiResult = ScrapeEndpointsResult;
using Scrape2PostmanOptions = ScrapeEndpointsOptions;
using Scrape2PostmanResult = ScrapeEndpointsResult;

bool is_api_path(const std::string& path,
                 const std::vector<std::string>& patterns);

// Reports whether `path` looks like static-asset gunk rather than a proper
// API endpoint: a known asset suffix (".js", ".css", ".png", ...) or an
// asset-directory marker ("/static/", "/fonts/", ...). Matching is
// case-insensitive on the path without query or fragment. Heuristic.
bool is_garbage_path(const std::string& path,
                     const std::vector<std::string>& patterns);

std::vector<DiscoveredEndpoint> filter_to_api(
    const std::vector<DiscoveredEndpoint>& endpoints,
    const ScrapeEndpointsOptions& options);

AssistantBrowserHandoff detect_assistant_browser_need(
    const Session& session, const ScrapeEndpointsResult& result,
    const ScrapeEndpointsOptions& options);

void probe_spa(Session& session, const ScrapeEndpointsOptions& options);

ScrapeEndpointsResult scrape_from_document(
    const Document& document, const ScrapeEndpointsOptions& options,
    const Redactor& redactor = Redactor());

ScrapeEndpointsResult scrape_from_session(
    Session& session, const ScrapeEndpointsOptions& options);

ScrapeEndpointsResult scrape(Session& session,
                             const ScrapeEndpointsOptions& options);

std::string render_scrape_yaml(const std::vector<ResolvedEndpoint>& resolved,
                               const ScrapeEndpointsOptions& options,
                               const Redactor& redactor);

std::string render_postman_json(
    const std::vector<ResolvedEndpoint>& resolved,
    const ScrapeEndpointsOptions& options = {},
    const Redactor& redactor = Redactor());

}  // namespace prowsetk::plugins::scrape_endpoints

namespace prowsetk::plugins::scrape2oapi {
using Scrape2OapiOptions = scrape_endpoints::ScrapeEndpointsOptions;
using Scrape2OapiResult = scrape_endpoints::ScrapeEndpointsResult;
using AssistantBrowserHandoff = scrape_endpoints::AssistantBrowserHandoff;
using ResolvedEndpoint = scrape_endpoints::ResolvedEndpoint;
using scrape_endpoints::detect_assistant_browser_need;
using scrape_endpoints::default_garbage_patterns;
using scrape_endpoints::filter_to_api;
using scrape_endpoints::is_api_path;
using scrape_endpoints::is_garbage_path;
using scrape_endpoints::render_scrape_yaml;
using scrape_endpoints::scrape;
using scrape_endpoints::scrape_from_document;
using scrape_endpoints::scrape_from_session;
}  // namespace prowsetk::plugins::scrape2oapi

namespace prowsetk::plugins::scrape2postman {
using Scrape2PostmanOptions = scrape_endpoints::ScrapeEndpointsOptions;
using Scrape2PostmanResult = scrape_endpoints::ScrapeEndpointsResult;
using ResolvedEndpoint = scrape_endpoints::ResolvedEndpoint;
using scrape_endpoints::render_postman_json;
inline Scrape2PostmanResult scrape_from_document(
    const Document& document, const Scrape2PostmanOptions& options = {},
    const Redactor& redactor = Redactor()) {
    return scrape_endpoints::scrape_from_document(document, options, redactor);
}
inline Scrape2PostmanResult scrape_from_session(
    Session& session, const Scrape2PostmanOptions& options = {}) {
    return scrape_endpoints::scrape_from_session(session, options);
}
inline Scrape2PostmanResult scrape(
    Session& session, const Scrape2PostmanOptions& options = {}) {
    return scrape_endpoints::scrape(session, options);
}
}  // namespace prowsetk::plugins::scrape2postman

#endif  // PROWSETK_PLUGINS_SCRAPE_ENDPOINTS_HPP
