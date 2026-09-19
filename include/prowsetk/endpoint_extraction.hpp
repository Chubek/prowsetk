#ifndef PROWSETK_ENDPOINT_EXTRACTION_HPP
#define PROWSETK_ENDPOINT_EXTRACTION_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "prowsetk/document.hpp"
#include "prowsetk/event.hpp"
#include "prowsetk/redaction.hpp"

namespace prowsetk {

struct EndpointExtractionOptions {
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
    // When true, scrape *all* link/resource endpoints within the page
    // regardless of API-like path markers. Anchor hrefs that do not look
    // like /api, /v1, etc. are emitted with confidence 0.60 so they pass
    // the default minimum_confidence filter. Resource URLs (script/img etc.)
    // are also emitted. Default false preserves the historic
    // API-focused filtering (see test_endpoint_extraction.IgnoresNonApiLinks).
    bool scrape_all_paths = false;
};

// One discovered endpoint plus the provenance required by README
// "Endpoint Extraction": discovery method, source, and confidence.
struct DiscoveredEndpoint {
    std::string url;
    std::string path;
    std::string method = "get";
    std::string source;
    std::string discovery_method;
    double confidence = 0.0;
    std::vector<std::string> parameters;
    std::string request_content_type;
    std::string response_content_type;
    std::vector<std::string> notes;
};

struct EndpointExtractionResult {
    std::string openapi_yaml;
    std::vector<DiscoveredEndpoint> endpoints;
    std::vector<std::string> warnings;
};

// Heuristic endpoint discovery. Never presents inferred values as
// authoritative; every endpoint carries provenance and confidence.
class EndpointExtractor {
public:
    EndpointExtractor();
    explicit EndpointExtractor(EndpointExtractionOptions options);

    EndpointExtractionResult extract(const Document& document) const;

    // Records an observed request/response pair so that network-observed
    // endpoints can be included with high confidence.
    void observe(std::string method, std::string url, int status,
                 std::string content_type = {});

    // Feeds an external script's text into discovery so that `fetch` and
    // `XMLHttpRequest` calls in external scripts are inspected (README
    // "Endpoint Extraction": inline and external JavaScript).
    void observe_script(std::string source_url, std::string_view script_text);

    // Optional event channel. When set, extract() emits an EndpointDiscovered
    // event for every endpoint that passes the confidence threshold.
    void set_event_dispatcher(EventDispatcher* dispatcher);

    const EndpointExtractionOptions& options() const noexcept { return options_; }

private:
    EndpointExtractionOptions options_;
    std::vector<DiscoveredEndpoint> observed_;
    std::vector<DiscoveredEndpoint> observed_scripts_;
    EventDispatcher* event_dispatcher_ = nullptr;
};

// Renders a deterministic OpenAPI 3.x YAML document from discovered endpoints.
// The output marks inferred values, preserves provenance, and redacts secrets.
std::string render_openapi_yaml(const std::vector<DiscoveredEndpoint>& endpoints,
                                const EndpointExtractionOptions& options,
                                const Redactor& redactor);

}  // namespace prowsetk

#endif  // PROWSETK_ENDPOINT_EXTRACTION_HPP
