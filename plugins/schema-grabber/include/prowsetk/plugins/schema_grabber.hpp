#ifndef PROWSETK_PLUGINS_SCHEMA_GRABBER_HPP
#define PROWSETK_PLUGINS_SCHEMA_GRABBER_HPP

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "prowsetk/browser.hpp"
#include "prowsetk/document.hpp"
#include "prowsetk/endpoint_extraction.hpp"
#include "prowsetk/redaction.hpp"

namespace prowsetk::plugins::schema_grabber {

// Options for schema reverse-engineering. The spec is declarative: Lua
// callers provide a table, C++ converts it to this struct. Secrets are never
// logged; examples derived from secret-bearing names/values are redacted in
// output by default. All inference is heuristic and marked as such.
struct SchemaGrabberOptions {
    // Source document. `html` switches to an offline run (session.load_html);
    // otherwise `url` is navigated when set. When grabbing from an existing
    // session document both may stay empty.
    std::string url;
    std::string html;
    std::string base_url;

    // Heuristic markers for suspected internal APIs. Mirrors scrape-endpoints
    // so the two plugins agree on what "API-like" means. An explicit non-GET
    // method is additionally kept regardless of path.
    std::vector<std::string> api_patterns = {"/api", "/v1", "/v2", "/v3",
                                             "/graphql", "/rest", "/internal",
                                             "/data", "/hotel/hoteladmin",
                                             "/partner-settings",
                                             "/telemetry", "challenge",
                                             "/beacon", "/collect"};
    bool require_api_pattern = true;

    // Discovery options forwarded to EndpointExtractor.
    bool inspect_scripts = true;
    bool infer_schemas = true;
    bool include_provenance = true;
    bool include_examples = true;
    bool redact_secrets = true;
    double minimum_confidence = 0.50;
    std::string openapi_version = "3.1.0";

    // When true (session paths only), each GET endpoint is probed once with a
    // host-mediated GET through the owning Session so the response body can be
    // reverse-engineered into a response schema. POST/PUT/PATCH endpoints are
    // never probed with their own method (side effects); their responses stay
    // heuristic unless a body was already resolved alongside scrape-endpoints.
    bool probe_get_responses = true;
    std::uint32_t max_probe_requests = 32;
    bool allow_cross_origin = false;

    // Inference budgets. Bodies larger than max_body_bytes are truncated
    // before inference and marked truncated.
    std::size_t max_body_bytes = 256u * 1024u;
    std::uint32_t max_properties = 64;
    std::uint32_t max_depth = 4;
    std::size_t max_example_chars = 256;

    std::string collection_name = "Discovered API (schema-grabber)";
};

// One inferred JSON value type.
struct JsonField {
    std::string name;
    std::string type = "string";  // string|integer|number|boolean|object|array|null
    std::string format;           // e.g. email, uri, uuid, date-time (heuristic)
    bool required = false;
    std::string example;
    std::vector<JsonField> properties;  // when type == object
    std::string items_type;             // when type == array
    std::vector<JsonField> items_properties;  // object array elements
    std::string provenance;               // e.g. json-response, form-field
    bool sensitive = false;
};

// An inferred object schema for a request or response body.
struct InferredSchema {
    std::string type = "object";
    std::vector<JsonField> properties;
    std::vector<std::string> required;
    std::string provenance;
    double confidence = 0.60;
    bool truncated = false;
};

// A reverse-engineered query parameter: name plus inferred scalar type,
// example value, and whether the name looks secret-bearing.
struct QueryParam {
    std::string name;
    std::string type = "string";
    bool required = false;
    std::string example;
    bool sensitive = false;
};

// A templated path segment (e.g. /users/123 -> /users/{id}).
struct PathParam {
    std::string name;
    std::string type = "string";
    std::string example;
};

// One observed or inferred response for an endpoint.
struct ResponseSchema {
    int status = 200;
    std::string content_type;
    std::optional<InferredSchema> schema;
    std::string provenance;
    bool observed = false;
};

// One endpoint with its reverse-engineered schemas.
struct EndpointSchema {
    DiscoveredEndpoint endpoint;
    std::string path_template;
    std::vector<QueryParam> query;
    std::vector<PathParam> path_params;
    std::vector<JsonField> form_fields;
    std::optional<InferredSchema> request_schema;
    std::string request_provenance;
    std::vector<ResponseSchema> responses;
    std::string response_provenance;
};

struct SchemaGrabberResult {
    std::vector<EndpointSchema> schemas;
    std::vector<DiscoveredEndpoint> endpoints;
    std::string openapi_yaml;
    std::string postman_json;
    std::vector<std::string> warnings;
    std::uint32_t probe_count = 0;

    void write_openapi_yaml(const std::filesystem::path& path) const;
    void write_postman_json(const std::filesystem::path& path) const;
};

// Minimal resolved-body view shared with scrape-endpoints output so the two
// plugins compose: pass scrape-endpoints ResolvedEndpoint bodies straight in
// and response schemas are reverse-engineered from observed bytes instead of
// re-probing. Header-only to avoid a plugin-to-plugin link dependency.
struct ResolvedBody {
    DiscoveredEndpoint endpoint;
    std::string final_url;
    int status = 0;
    std::string body;
    std::string content_type;
};

bool is_schema_api_path(const std::string& path,
                        const std::vector<std::string>& patterns);

std::vector<DiscoveredEndpoint> filter_to_api(
    const std::vector<DiscoveredEndpoint>& endpoints,
    const SchemaGrabberOptions& options);

// Rewrites volatile path segments (numeric ids, UUIDs, long hashes) to
// `{id}`-style templates. Returns the template; `params` receives one entry
// per templated segment with the original text as the example.
std::string templatize_path(const std::string& path,
                            std::vector<PathParam>* params = nullptr);

// Infers an OpenAPI scalar type name from an example string value.
std::string infer_scalar_type(const std::string& value);

// Splits the query string of `url` into typed query parameters. Sensitive
// names (per `redactor`) keep their type but lose their example when
// `redact` is true.
std::vector<QueryParam> query_params_for(const std::string& url,
                                         const Redactor& redactor,
                                         bool redact = true);

// Infers an object schema from a JSON body. Returns nullopt when the body is
// not JSON or is empty. Never throws on malformed input.
std::optional<InferredSchema> infer_json_schema(const std::string& body,
                                                const SchemaGrabberOptions& options,
                                                const Redactor& redactor,
                                                std::string provenance = "json-response");

// Finds form controls in `document` whose action+method match `endpoint` and
// returns them as typed fields (input types mapped to scalar types,
// `required` honored, secret-bearing names redacted).
std::vector<JsonField> form_fields_for(const Document& document,
                                       const DiscoveredEndpoint& endpoint,
                                       const SchemaGrabberOptions& options,
                                       const Redactor& redactor);

// Builds the request schema for one endpoint: form fields win for
// form-urlencoded POSTs, then JSON body hints found near the endpoint's path
// in inline scripts, else a generic inferred object when a content type is
// known.
std::optional<InferredSchema> request_schema_for(
    const DiscoveredEndpoint& endpoint, const Document* document,
    const SchemaGrabberOptions& options, const Redactor& redactor,
    std::string* provenance_out = nullptr);

// Enriches already-discovered endpoints (e.g. from scrape-endpoints) with
// request/response schemas. Entries in `bodies` are keyed by method+path and
// supply observed response bytes without network access.
std::vector<EndpointSchema> enrich_endpoints(
    const std::vector<DiscoveredEndpoint>& endpoints,
    const std::vector<ResolvedBody>& bodies, const Document* document,
    const SchemaGrabberOptions& options, const Redactor& redactor);

// Document variants never touch the network: response schemas stay heuristic
// (generic objects) unless `bodies` carry observed bytes.
SchemaGrabberResult grab_from_document(const Document& document,
                                       const SchemaGrabberOptions& options,
                                       const Redactor& redactor = Redactor());
SchemaGrabberResult grab_from_document_with_bodies(
    const Document& document, const std::vector<ResolvedBody>& bodies,
    const SchemaGrabberOptions& options, const Redactor& redactor = Redactor());

// Session variants run discovery through the owning Session and, when
// `probe_get_responses` is set, issue bounded host-mediated GET probes for
// GET endpoints to reverse-engineer response schemas.
SchemaGrabberResult grab_from_session(Session& session,
                                      const SchemaGrabberOptions& options);

// High-level helper: given a session and options that may contain an html
// override, load or navigate as required then run grab_from_session.
SchemaGrabberResult grab(Session& session, const SchemaGrabberOptions& options);

// Deterministic OpenAPI 3.x rendering with per-endpoint query/path
// parameters plus request and response schemas. Inferred values carry
// `x-inferred`, provenance, and confidence; secrets stay redacted.
std::string render_schema_yaml(const std::vector<EndpointSchema>& schemas,
                               const SchemaGrabberOptions& options,
                               const Redactor& redactor);

// Deterministic Postman 2.1 collection rendering with query parameters,
// path variables, request bodies (urlencoded fields or JSON examples), and
// saved example responses when response bodies were observed.
std::string render_schema_postman_json(const std::vector<EndpointSchema>& schemas,
                                       const SchemaGrabberOptions& options,
                                       const Redactor& redactor);

}  // namespace prowsetk::plugins::schema_grabber

#endif  // PROWSETK_PLUGINS_SCHEMA_GRABBER_HPP
