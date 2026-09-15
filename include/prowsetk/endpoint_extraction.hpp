#ifndef PROWSETK_ENDPOINT_EXTRACTION_HPP
#define PROWSETK_ENDPOINT_EXTRACTION_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "prowsetk/document.hpp"
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
};

// One discovered endpoint plus the provenance required by README
// "Endpoint Extraction": discovery method, source, and confidence.
struct DiscoveredEndpoint {
    std::string url;
    std::string path;
    std::string method = "GET";
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

    const EndpointExtractionOptions& options() const noexcept { return options_; }

private:
    EndpointExtractionOptions options_;
    std::vector<DiscoveredEndpoint> observed_;
};

// Renders a deterministic OpenAPI 3.x YAML document from discovered endpoints.
// The output marks inferred values, preserves provenance, and redacts secrets.
std::string render_openapi_yaml(const std::vector<DiscoveredEndpoint>& endpoints,
                                const EndpointExtractionOptions& options,
                                const Redactor& redactor);

}  // namespace prowsetk

#endif  // PROWSETK_ENDPOINT_EXTRACTION_HPP
