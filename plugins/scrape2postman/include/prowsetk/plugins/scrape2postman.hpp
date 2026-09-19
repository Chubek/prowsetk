#ifndef PROWSETK_PLUGINS_SCRAPE2POSTMAN_HPP
#define PROWSETK_PLUGINS_SCRAPE2POSTMAN_HPP

#include <filesystem>
#include "prowsetk/plugins/scrape2oapi.hpp"

namespace prowsetk::plugins::scrape2postman {

// Reuses scrape2oapi's discovery/filter/host-mediated resolution contract.
// openapi_version is inherited for source compatibility but does not affect
// the Postman Collection v2.1 format. No authentication or body samples export.
struct Scrape2PostmanOptions : scrape2oapi::Scrape2OapiOptions {
    std::string collection_name = "Discovered API (scrape2postman)";
};

using ResolvedEndpoint = scrape2oapi::ResolvedEndpoint;

struct Scrape2PostmanResult {
    // These are the original discovery records, potentially containing secrets.
    // Only postman_json is suitable for sharing with redaction enabled.
    std::vector<ResolvedEndpoint> resolved;
    std::vector<DiscoveredEndpoint> endpoints;
    std::vector<std::string> warnings;
    std::string postman_json;

    // Parent directory must exist. Throws on open, write, or close failure.
    void write_postman_json(const std::filesystem::path& path) const;
};

std::string render_postman_json(const std::vector<ResolvedEndpoint>& resolved,
                               const Scrape2PostmanOptions& options = {},
                               const Redactor& redactor = Redactor());
Scrape2PostmanResult scrape_from_document(
    const Document& document, const Scrape2PostmanOptions& options = {},
    const Redactor& redactor = Redactor());
Scrape2PostmanResult scrape_from_session(
    Session& session, const Scrape2PostmanOptions& options = {});
// html takes precedence over url; an existing document can also be scraped.
Scrape2PostmanResult scrape(Session& session,
                          const Scrape2PostmanOptions& options = {});

}  // namespace prowsetk::plugins::scrape2postman
#endif
