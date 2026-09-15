#include "prowsetk/redaction.hpp"

#include <algorithm>
#include <cctype>

#include "prowsetk/url.hpp"

namespace prowsetk {
namespace {

std::string to_lower(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](char c) {
        return static_cast<char>(
            std::tolower(static_cast<unsigned char>(c)));
    });
    return result;
}

bool contains_case_insensitive(const std::vector<std::string>& names,
                               std::string_view candidate) {
    const std::string lowered = to_lower(candidate);
    return std::any_of(names.begin(), names.end(),
                       [&lowered](const std::string& name) {
                           return to_lower(name) == lowered;
                       });
}

}  // namespace

RedactionPolicy RedactionPolicy::defaults() {
    RedactionPolicy policy;
    policy.header_names = {"authorization",
                           "proxy-authorization",
                           "cookie",
                           "set-cookie",
                           "x-api-key",
                           "api-key",
                           "x-auth-token",
                           "x-csrf-token"};
    policy.query_parameter_names = {"token",
                                    "access_token",
                                    "refresh_token",
                                    "id_token",
                                    "api_key",
                                    "apikey",
                                    "password",
                                    "passwd",
                                    "secret",
                                    "client_secret",
                                    "session",
                                    "sessionid",
                                    "auth",
                                    "signature",
                                    "sig"};
    return policy;
}

Redactor::Redactor() : policy_(RedactionPolicy::defaults()) {}

Redactor::Redactor(RedactionPolicy policy) : policy_(std::move(policy)) {}

bool Redactor::is_sensitive_header(std::string_view name) const noexcept {
    return contains_case_insensitive(policy_.header_names, name);
}

bool Redactor::is_sensitive_query_parameter(
    std::string_view name) const noexcept {
    return contains_case_insensitive(policy_.query_parameter_names, name);
}

std::string Redactor::redact_header(std::string_view name,
                                    std::string_view value) const {
    if (is_sensitive_header(name)) {
        return policy_.replacement;
    }
    return std::string(value);
}

std::string Redactor::redact_url(std::string_view url) const {
    Url parsed;
    try {
        parsed = parse_url(url);
    } catch (...) {
        return std::string(url);
    }
    if (!parsed.has_query) {
        return std::string(url);
    }

    std::string rebuilt;
    std::size_t start = 0;
    bool first = true;
    while (start <= parsed.query.size()) {
        const auto amp = parsed.query.find('&', start);
        const std::string pair =
            amp == std::string::npos
                ? parsed.query.substr(start)
                : parsed.query.substr(start, amp - start);
        const auto eq = pair.find('=');
        const std::string key = eq == std::string::npos ? pair : pair.substr(0, eq);
        const std::string value =
            eq == std::string::npos ? std::string() : pair.substr(eq + 1);
        if (!first) {
            rebuilt += "&";
        }
        first = false;
        if (is_sensitive_query_parameter(key)) {
            rebuilt += key + "=" + policy_.replacement;
        } else {
            rebuilt += pair;
        }
        if (amp == std::string::npos) {
            break;
        }
        start = amp + 1;
    }
    parsed.query = rebuilt;
    return parsed.to_string();
}

std::vector<std::pair<std::string, std::string>> Redactor::redact_headers(
    const std::vector<std::pair<std::string, std::string>>& headers) const {
    std::vector<std::pair<std::string, std::string>> result;
    result.reserve(headers.size());
    for (const auto& [name, value] : headers) {
        result.emplace_back(name, redact_header(name, value));
    }
    return result;
}

}  // namespace prowsetk
