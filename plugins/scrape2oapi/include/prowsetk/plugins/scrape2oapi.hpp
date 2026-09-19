#ifndef PROWSETK_PLUGINS_SCRAPE2OAPI_HPP
#define PROWSETK_PLUGINS_SCRAPE2OAPI_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "prowsetk/browser.hpp"
#include "prowsetk/document.hpp"
#include "prowsetk/endpoint_extraction.hpp"
#include "prowsetk/redaction.hpp"

namespace prowsetk::plugins::scrape2oapi {

// Options that mirror the Lua spec table for scrape2oapi. The spec is
// declarative: Lua callers provide a table, C++ converts it to this struct.
struct Scrape2OapiOptions {
    // Source document. Exactly one of url / html must be supplied when scraping
    // via a Session. When scraping a Document directly, these are ignored.
    std::string url;
    std::string html;
    std::string base_url;

    // Endpoint discovery options forwarded to EndpointExtractor.
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

    // scrape2oapi-specific: heuristics for internal API detection.
    std::vector<std::string> api_patterns = {"/api", "/v1", "/v2", "/v3",
                                             "/graphql", "/rest", "/internal",
                                             "/data", "/hotel/hoteladmin",
                                             "/partner-settings"};
    bool require_api_pattern = true;

    // Chain resolution: when true, every discovered internal API endpoint is
    // fetched through the Session's host-mediated NetworkClient until the end
    // of the redirect chain is reached. Redirect chains are recorded as
    // provenance; JSON bodies that contain further API-like URLs are optionally
    // enqueued for recursive resolution up to max_depth.
    bool resolve_chain = false;
    bool follow_json_links = true;
    // Upper bound on network fetches during chain resolution.
    std::uint32_t max_resolve_requests = 64;
};

// One endpoint that has been resolved through the network. The original
// discovered URL is endpoint.url; final_url and chain capture the host-mediated
// redirect outcome. status and body are populated only when resolve_chain is
// true.
struct ResolvedEndpoint {
    DiscoveredEndpoint endpoint;
    std::string final_url;
    int status = 0;
    std::string body;
    std::string content_type;
    std::vector<std::string> redirect_chain;
    // When the response body is JSON and follow_json_links is enabled, any API
    // references extracted from the body are reported here.
    std::vector<std::string> json_discovered;
    std::string error;
};

// Full scrape result. endpoints is the filtered, deduplicated list; yaml is a
// deterministic OpenAPI 3.x document that redacts secrets by default and marks
// every path as inferred with provenance and confidence preserved.
struct Scrape2OapiResult {
    std::vector<ResolvedEndpoint> resolved;
    std::vector<DiscoveredEndpoint> endpoints;
    std::string openapi_yaml;
    std::vector<std::string> warnings;
};

bool is_api_path(const std::string& path,
                 const std::vector<std::string>& patterns);

// Filters endpoints to suspected internal APIs. When require_api_pattern is
// false every endpoint is retained; otherwise only paths that match one of the
// patterns (or the built-in heuristic markers) are kept.
std::vector<DiscoveredEndpoint> filter_to_api(
    const std::vector<DiscoveredEndpoint>& endpoints,
    const Scrape2OapiOptions& options);

// Core extraction helpers. Session variants may optionally resolve the chain
// through Session::request so all network access remains host-mediated and
// subject to the browser's policies. Document variants never touch the
// network.
Scrape2OapiResult scrape_from_document(const Document& document,
                                       const Scrape2OapiOptions& options,
                                       const Redactor& redactor = Redactor());

Scrape2OapiResult scrape_from_session(Session& session,
                                      const Scrape2OapiOptions& options);

// High-level helper used by the plugin entry and Lua binding: given a session
// and options that may contain an html override, load or navigate as required
// then run scrape_from_session. For offline runs no network request is issued.
Scrape2OapiResult scrape(Session& session, const Scrape2OapiOptions& options);

// Render helper that is also used by Lua bindings. When resolve_chain was
// enabled the per-endpoint x-prowsetk-chain extension is emitted.
std::string render_scrape_yaml(const std::vector<ResolvedEndpoint>& resolved,
                               const Scrape2OapiOptions& options,
                               const Redactor& redactor);

}  // namespace prowsetk::plugins::scrape2oapi

#endif  // PROWSETK_PLUGINS_SCRAPE2OAPI_HPP
