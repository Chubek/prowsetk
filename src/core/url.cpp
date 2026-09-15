#include "prowsetk/url.hpp"

#include <algorithm>
#include <cctype>

#include "prowsetk/error.hpp"

namespace prowsetk {
namespace {

bool is_scheme_start(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) != 0;
}

bool is_scheme_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '+' ||
           c == '-' || c == '.';
}

std::string to_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](char c) {
        return static_cast<char>(
            std::tolower(static_cast<unsigned char>(c)));
    });
    return value;
}

std::string remove_dot_segments(const std::string& input) {
    std::string output;
    std::string path = input;
    while (!path.empty()) {
        if (path.rfind("../", 0) == 0) {
            path.erase(0, 3);
        } else if (path.rfind("./", 0) == 0) {
            path.erase(0, 2);
        } else if (path.rfind("/./", 0) == 0) {
            path.erase(0, 2);
        } else if (path == "/.") {
            path = "/";
        } else if (path.rfind("/../", 0) == 0) {
            path.erase(0, 3);
            const auto slash = output.find_last_of('/');
            if (slash == std::string::npos) {
                output.clear();
            } else {
                output.erase(slash);
            }
        } else if (path == "/..") {
            path = "/";
            const auto slash = output.find_last_of('/');
            if (slash == std::string::npos) {
                output.clear();
            } else {
                output.erase(slash);
            }
        } else if (path == "." || path == "..") {
            path.clear();
        } else {
            std::size_t start = 0;
            if (path[0] == '/') {
                start = 1;
            }
            const auto next = path.find('/', start);
            if (next == std::string::npos) {
                output += path;
                path.clear();
            } else {
                output += path.substr(0, next);
                path.erase(0, next);
            }
        }
    }
    return output;
}

std::string merge_paths(const Url& base, const std::string& reference_path) {
    if (base.has_authority && base.path.empty()) {
        return "/" + reference_path;
    }
    const auto slash = base.path.find_last_of('/');
    if (slash == std::string::npos) {
        return reference_path;
    }
    return base.path.substr(0, slash + 1) + reference_path;
}

}  // namespace

std::string Url::origin() const {
    if (!has_authority) {
        return {};
    }
    std::string result = scheme + "://" + host;
    if (!port.empty()) {
        result += ":" + port;
    }
    return result;
}

std::string Url::to_string() const {
    std::string result;
    if (!scheme.empty()) {
        result += scheme + ":";
    }
    if (has_authority) {
        result += "//";
        if (!userinfo.empty()) {
            result += userinfo + "@";
        }
        result += host;
        if (!port.empty()) {
            result += ":" + port;
        }
    }
    result += path;
    if (has_query) {
        result += "?" + query;
    }
    if (has_fragment) {
        result += "#" + fragment;
    }
    return result;
}

Url parse_url(std::string_view input) {
    Url url;
    std::string rest(input);

    const auto colon = rest.find(':');
    if (colon != std::string::npos && colon > 0 && is_scheme_start(rest[0])) {
        bool valid_scheme = true;
        for (std::size_t i = 0; i < colon; ++i) {
            if (!is_scheme_char(rest[i])) {
                valid_scheme = false;
                break;
            }
        }
        if (valid_scheme) {
            url.scheme = to_lower(rest.substr(0, colon));
            rest.erase(0, colon + 1);
        }
    }

    if (rest.rfind("//", 0) == 0) {
        rest.erase(0, 2);
        url.has_authority = true;
        const auto end = rest.find_first_of("/?#");
        std::string authority =
            end == std::string::npos ? rest : rest.substr(0, end);
        rest = end == std::string::npos ? std::string() : rest.substr(end);

        const auto at = authority.find('@');
        if (at != std::string::npos) {
            url.userinfo = authority.substr(0, at);
            authority.erase(0, at + 1);
        }

        if (!authority.empty() && authority[0] == '[') {
            const auto close = authority.find(']');
            if (close == std::string::npos) {
                throw Error(ErrorCode::InvalidUrl, "unterminated IPv6 host");
            }
            url.host = authority.substr(0, close + 1);
            const auto after = close + 1;
            if (after < authority.size() && authority[after] == ':') {
                url.port = authority.substr(after + 1);
            }
        } else {
            const auto port_sep = authority.find(':');
            if (port_sep != std::string::npos) {
                url.host = authority.substr(0, port_sep);
                url.port = authority.substr(port_sep + 1);
            } else {
                url.host = authority;
            }
        }
        url.host = to_lower(url.host);
    }

    const auto hash = rest.find('#');
    if (hash != std::string::npos) {
        url.fragment = rest.substr(hash + 1);
        url.has_fragment = true;
        rest.erase(hash);
    }

    const auto question = rest.find('?');
    if (question != std::string::npos) {
        url.query = rest.substr(question + 1);
        url.has_query = true;
        rest.erase(question);
    }

    url.path = rest;
    return url;
}

Url resolve_url(const Url& base, std::string_view reference) {
    Url ref = parse_url(reference);
    Url target;

    if (!ref.scheme.empty()) {
        target = ref;
        target.path = remove_dot_segments(ref.path);
        target.has_query = ref.has_query;
        return target;
    }

    target.scheme = base.scheme;
    if (ref.has_authority) {
        target.has_authority = true;
        target.userinfo = ref.userinfo;
        target.host = ref.host;
        target.port = ref.port;
        target.path = remove_dot_segments(ref.path);
        target.query = ref.query;
        target.has_query = ref.has_query;
    } else {
        target.has_authority = base.has_authority;
        target.userinfo = base.userinfo;
        target.host = base.host;
        target.port = base.port;
        if (ref.path.empty()) {
            target.path = base.path;
            target.query = ref.has_query ? ref.query : base.query;
            target.has_query = ref.has_query ? true : base.has_query;
        } else {
            if (ref.path[0] == '/') {
                target.path = remove_dot_segments(ref.path);
            } else {
                target.path =
                    remove_dot_segments(merge_paths(base, ref.path));
            }
            target.query = ref.query;
            target.has_query = ref.has_query;
        }
    }
    target.fragment = ref.fragment;
    target.has_fragment = ref.has_fragment;
    return target;
}

std::string resolve_url(std::string_view base, std::string_view reference) {
    const Url base_url = parse_url(base);
    const Url target = resolve_url(base_url, reference);
    Url normalized = target;
    normalized.path = remove_dot_segments(normalized.path);
    if (normalized.has_authority && normalized.path.empty()) {
        normalized.path = "/";
    }
    return normalized.to_string();
}

std::string normalize_url(std::string_view input) {
    Url url = parse_url(input);
    url.scheme = to_lower(url.scheme);
    url.host = to_lower(url.host);
    url.path = remove_dot_segments(url.path);
    if ((url.scheme == "http" && url.port == "80") ||
        (url.scheme == "https" && url.port == "443")) {
        url.port.clear();
    }
    if (url.has_authority && url.path.empty()) {
        url.path = "/";
    }
    return url.to_string();
}

}  // namespace prowsetk
