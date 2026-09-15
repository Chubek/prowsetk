#ifndef PROWSETK_URL_HPP
#define PROWSETK_URL_HPP

#include <string>
#include <string_view>

namespace prowsetk {

// A parsed absolute or relative URL. `has_authority` distinguishes "http://"
// style URLs from opaque/scheme-only references.
struct Url {
    std::string scheme;
    std::string userinfo;
    std::string host;
    std::string port;
    std::string path;
    std::string query;
    std::string fragment;
    bool has_authority = false;

    bool is_absolute() const noexcept { return !scheme.empty(); }
    bool has_host() const noexcept { return has_authority && !host.empty(); }

    std::string origin() const;
    std::string to_string() const;
};

// Parses `input`. Throws Error(InvalidUrl) when the reference is malformed.
Url parse_url(std::string_view input);

// Resolves `reference` against `base` per RFC 3986 section 5.
Url resolve_url(const Url& base, std::string_view reference);

// Convenience wrapper: parse `base`, resolve `reference`, normalize the result.
std::string resolve_url(std::string_view base, std::string_view reference);

// Removes dot segments and lowercases the scheme/host.
std::string normalize_url(std::string_view input);

}  // namespace prowsetk

#endif  // PROWSETK_URL_HPP
