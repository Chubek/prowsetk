#ifndef PROWSETK_REDACTION_HPP
#define PROWSETK_REDACTION_HPP

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace prowsetk {

// Names that are considered sensitive by default. Secrets must be redacted in
// logs and generated OpenAPI output unless a caller explicitly opts out.
struct RedactionPolicy {
    std::vector<std::string> header_names;
    std::vector<std::string> query_parameter_names;
    std::string replacement = "[REDACTED]";

    static RedactionPolicy defaults();
};

class Redactor {
public:
    Redactor();
    explicit Redactor(RedactionPolicy policy);

    const RedactionPolicy& policy() const noexcept { return policy_; }

    bool is_sensitive_header(std::string_view name) const noexcept;
    bool is_sensitive_query_parameter(std::string_view name) const noexcept;

    // Returns the value unchanged unless the header name is sensitive.
    std::string redact_header(std::string_view name, std::string_view value) const;

    // Rewrites the query string of `url`, replacing sensitive parameter values.
    std::string redact_url(std::string_view url) const;

    std::vector<std::pair<std::string, std::string>> redact_headers(
        const std::vector<std::pair<std::string, std::string>>& headers) const;

private:
    RedactionPolicy policy_;
};

}  // namespace prowsetk

#endif  // PROWSETK_REDACTION_HPP
