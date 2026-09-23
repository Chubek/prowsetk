#ifndef PROWSETK_PLUGINS_RESTFUL_RESOLVER_HPP
#define PROWSETK_PLUGINS_RESTFUL_RESOLVER_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "prowsetk/browser.hpp"
#include "prowsetk/document.hpp"
#include "prowsetk/endpoint_extraction.hpp"
#include "prowsetk/redaction.hpp"

namespace prowsetk::plugins::restful_resolver {

// Options for iterative RESTful resolution. The spec is declarative: Lua
// callers provide a table, C++ converts it to this struct. Secrets are never
// logged; URLs are redacted in output by default.
struct RestfulResolverOptions {
    // Source document. `html` switches to an offline run (session.load_html);
    // otherwise `url` is navigated when set. When resolving from an existing
    // session document both may stay empty.
    std::string url;
    std::string html;
    std::string base_url;

    // Heuristic markers for suspected internal APIs. Mirrors scrape2oapi so
    // the two plugins agree on what "API-like" means. Beacon/challenge
    // markers keep telemetry-style POST endpoints (e.g.
    // `/__challenge_.../telemetry`) API-like. An explicit non-GET method is
    // additionally kept regardless of path (see filter_to_api).
    std::vector<std::string> api_patterns = {"/api", "/v1", "/v2", "/v3",
                                             "/graphql", "/rest", "/internal",
                                             "/data", "/hotel/hoteladmin",
                                             "/partner-settings",
                                             "/telemetry", "challenge",
                                             "/beacon", "/collect"};
    bool require_api_pattern = true;

    // Resolution budget. Each BFS round fetches one URL through the owning
    // Session (host-mediated NetworkClient); newly discovered API URLs are
    // enqueued for the next round until a POST endpoint is found or the
    // budget is exhausted.
    std::uint32_t max_rounds = 4;
    std::uint32_t max_requests = 64;
    bool follow_json_links = true;
    bool allow_cross_origin = false;

    // Discovery options forwarded to EndpointExtractor.
    bool inspect_scripts = true;
    bool infer_schemas = true;
    bool include_provenance = true;
    bool redact_secrets = true;
    double minimum_confidence = 0.45;
    std::string openapi_version = "3.1.0";
};

// One endpoint probed through the network. `endpoint.url` is the discovered
// URL; `final_url` and `redirect_chain` capture the host-mediated outcome.
// `round` is the BFS round (0 = seed document). `status` is 0 when no fetch
// was attempted (seed endpoints carried over without resolution).
struct ResolvedCall {
    DiscoveredEndpoint endpoint;
    std::string final_url;
    int status = 0;
    std::string content_type;
    std::vector<std::string> redirect_chain;
    std::string error;
    std::uint32_t round = 0;
};

// Full resolution result. `is_complete` means a full RESTful surface was
// discovered: at least one GET and at least one POST endpoint. Discovery is
// heuristic; every endpoint carries provenance and confidence and the YAML
// marks inferred values.
struct RestfulResolverResult {
    std::vector<ResolvedCall> resolved;
    std::vector<DiscoveredEndpoint> endpoints;
    bool has_get = false;
    bool has_post = false;
    bool is_complete = false;
    std::uint32_t rounds_used = 0;
    std::uint32_t request_count = 0;
    std::string openapi_yaml;
    std::vector<std::string> warnings;
};

bool is_restful_api_path(const std::string& path,
                         const std::vector<std::string>& patterns);

// Filters endpoints to suspected internal APIs. When require_api_pattern is
// false every endpoint is retained; otherwise paths matching one of the
// patterns (or the built-in heuristic markers) are kept, as is any endpoint
// with an explicit non-GET method (POST forms, fetch/XHR POSTs, beacons).
std::vector<DiscoveredEndpoint> filter_to_api(
    const std::vector<DiscoveredEndpoint>& endpoints,
    const RestfulResolverOptions& options);

bool has_post_endpoint(const std::vector<DiscoveredEndpoint>& endpoints);
bool has_get_endpoint(const std::vector<DiscoveredEndpoint>& endpoints);

// Document variants never touch the network: they report seed endpoints and
// whether the seed document alone already exposes a full RESTful surface.
RestfulResolverResult resolve_from_document(
    const Document& document, const RestfulResolverOptions& options,
    const Redactor& redactor = Redactor());

// Session variants resolve iteratively through Session::request so all
// network access remains host-mediated and subject to browser policies.
RestfulResolverResult resolve_from_session(
    Session& session, const RestfulResolverOptions& options);

// High-level helper: given a session and options that may contain an html
// override, load or navigate as required then run resolve_from_session. For
// offline runs no network request is issued beyond the resolution budget.
RestfulResolverResult resolve(Session& session,
                              const RestfulResolverOptions& options);

// Deterministic OpenAPI 3.x rendering of the resolved endpoints. Emits the
// x-prowsetk-restful extension (has_post/is_complete/rounds) alongside the
// standard provenance/confidence metadata.
std::string render_resolver_yaml(const std::vector<ResolvedCall>& resolved,
                                 const RestfulResolverResult& summary,
                                 const RestfulResolverOptions& options,
                                 const Redactor& redactor);

}  // namespace prowsetk::plugins::restful_resolver

#endif  // PROWSETK_PLUGINS_RESTFUL_RESOLVER_HPP
