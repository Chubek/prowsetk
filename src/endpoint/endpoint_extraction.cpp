#include "prowsetk/endpoint_extraction.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <sstream>

#include "prowsetk/url.hpp"

namespace prowsetk {
namespace {

std::string to_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    });
    return value;
}

bool looks_like_api_path(const std::string& path) {
    static const char* const markers[] = {"/api", "/v1", "/v2", "/v3",
                                          "/graphql", "/rest", "/rpc",
                                          "/json", ".json", "/data"};
    for (const char* marker : markers) {
        if (path.find(marker) != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool is_http_method(std::string_view method) {
    static const char* const methods[] = {"get",  "post", "put", "patch",
                                          "delete", "head", "options"};
    for (const char* known : methods) {
        if (method == known) {
            return true;
        }
    }
    return false;
}

std::string normalize_method(std::string method, std::string fallback = "get") {
    if (method.empty()) {
        return fallback;
    }
    method = to_lower(std::move(method));
    return is_http_method(method) ? method : to_lower(std::move(fallback));
}

std::string method_from_form(const Element& form) {
    std::string method = form.attribute("method");
    if (method.empty()) {
        return "get";
    }
    method = to_lower(method);
    return method == "post" ? "post" : "get";
}

void append_unique(std::vector<std::string>& values, std::string value) {
    if (value.empty()) {
        return;
    }
    if (std::find(values.begin(), values.end(), value) == values.end()) {
        values.push_back(std::move(value));
    }
}

void append_unique_all(std::vector<std::string>& values,
                       const std::vector<std::string>& incoming) {
    for (const auto& value : incoming) {
        append_unique(values, value);
    }
}

std::vector<std::string> query_parameters(std::string_view query) {
    std::vector<std::string> parameters;
    std::size_t start = 0;
    while (start <= query.size()) {
        const auto amp = query.find('&', start);
        const std::string pair =
            amp == std::string_view::npos
                ? std::string(query.substr(start))
                : std::string(query.substr(start, amp - start));
        const auto eq = pair.find('=');
        append_unique(parameters,
                      eq == std::string::npos ? pair : pair.substr(0, eq));
        if (amp == std::string_view::npos) {
            break;
        }
        start = amp + 1;
    }
    return parameters;
}

std::vector<std::string> form_parameters(const Element& form) {
    std::vector<std::string> parameters;
    for (const auto& control :
         form.query_selector_all("input[name], select[name], textarea[name]")) {
        append_unique(parameters, control->attribute("name"));
    }
    return parameters;
}

// Extracts (method, url) pairs from inline JavaScript. This is a heuristic
// scanner, not a JavaScript parser.
std::vector<std::pair<std::string, std::string>> script_endpoints(
    std::string_view script) {
    std::vector<std::pair<std::string, std::string>> endpoints;
    const auto read_quoted = [&script](std::size_t start,
                                       std::string& out) -> std::size_t {
        if (start >= script.size()) {
            return std::string::npos;
        }
        const char quote = script[start];
        if (quote != '\'' && quote != '"' && quote != '`') {
            return std::string::npos;
        }
        for (std::size_t i = start + 1; i < script.size(); ++i) {
            if (script[i] == '\\' && i + 1 < script.size()) {
                out.push_back(script[i + 1]);
                ++i;
                continue;
            }
            if (script[i] == quote) {
                return i + 1;
            }
            out.push_back(script[i]);
        }
        out.clear();
        return std::string::npos;
    };

    const auto fetch_options_method = [&script, &read_quoted](
                                          std::size_t options_start,
                                          std::size_t options_end) {
        const std::string fallback = "get";
        if (options_start == std::string_view::npos ||
            options_end == std::string_view::npos ||
            options_start >= options_end) {
            return fallback;
        }
        const std::string_view options =
            script.substr(options_start, options_end - options_start);
        std::size_t method = options.find("method");
        if (method == std::string_view::npos) {
            return fallback;
        }
        method += 6;
        while (method < options.size() &&
               std::isspace(static_cast<unsigned char>(options[method]))) {
            ++method;
        }
        if (method >= options.size() || options[method] != ':') {
            return fallback;
        }
        ++method;
        while (method < options.size() &&
               std::isspace(static_cast<unsigned char>(options[method]))) {
            ++method;
        }
        std::string value;
        const std::size_t absolute = options_start + method;
        if (read_quoted(absolute, value) == std::string::npos) {
            return fallback;
        }
        return normalize_method(std::move(value), fallback);
    };

    std::size_t cursor = 0;
    while ((cursor = script.find("fetch(", cursor)) != std::string_view::npos) {
        std::size_t i = cursor + 6;
        while (i < script.size() && std::isspace(static_cast<unsigned char>(script[i]))) {
            ++i;
        }
        std::string url;
        const std::size_t after_url = read_quoted(i, url);
        if (after_url != std::string::npos) {
            const auto close = script.find(')', after_url);
            const auto comma = script.find(',', after_url);
            const std::string method =
                comma != std::string_view::npos && close != std::string_view::npos &&
                        comma < close
                    ? fetch_options_method(comma + 1, close)
                    : "get";
            endpoints.emplace_back(normalize_method(method), url);
        }
        cursor += 6;
    }

    cursor = 0;
    while ((cursor = script.find(".open(", cursor)) != std::string_view::npos) {
        std::size_t i = cursor + 6;
        while (i < script.size() &&
               std::isspace(static_cast<unsigned char>(script[i]))) {
            ++i;
        }
        std::string method;
        if (read_quoted(i, method) == std::string::npos) {
            cursor += 6;
            continue;
        }
        i = script.find(',', i);
        if (i == std::string_view::npos) {
            cursor += 6;
            continue;
        }
        ++i;
        while (i < script.size() &&
               std::isspace(static_cast<unsigned char>(script[i]))) {
            ++i;
        }
        std::string url;
        if (read_quoted(i, url) != std::string::npos) {
            endpoints.emplace_back(normalize_method(std::move(method)), url);
        }
        cursor += 6;
    }

    return endpoints;
}

std::string yaml_quote(std::string_view value) {
    std::string result = "'";
    for (char c : value) {
        if (c == '\'') {
            result += "''";
        } else if (c == '\n') {
            result += "\\n";
        } else {
            result.push_back(c);
        }
    }
    result += "'";
    return result;
}

std::string operation_id(const std::string& method, const std::string& path) {
    std::string id = to_lower(method) + "_" + path;
    for (char& c : id) {
        if (!std::isalnum(static_cast<unsigned char>(c))) {
            c = '_';
        }
    }
    while (id.find("__") != std::string::npos) {
        id.replace(id.find("__"), 2, "_");
    }
    if (!id.empty() && id.front() == '_') {
        id.erase(id.begin());
    }
    if (!id.empty() && id.back() == '_') {
        id.pop_back();
    }
    return id;
}

std::string format_confidence(double confidence) {
    std::ostringstream stream;
    stream.setf(std::ios::fixed);
    stream.precision(2);
    stream << confidence;
    return stream.str();
}

}  // namespace

EndpointExtractor::EndpointExtractor() = default;

EndpointExtractor::EndpointExtractor(EndpointExtractionOptions options)
    : options_(std::move(options)) {}

void EndpointExtractor::observe(std::string method, std::string url, int status,
                                std::string content_type) {
    DiscoveredEndpoint endpoint;
    endpoint.url = std::move(url);
    endpoint.method = normalize_method(std::move(method));
    endpoint.discovery_method = "observed-network";
    endpoint.confidence = 0.95;
    endpoint.source = endpoint.url;
    endpoint.response_content_type = std::move(content_type);
    try {
        const Url parsed = parse_url(endpoint.url);
        endpoint.path = parsed.path.empty() ? "/" : parsed.path;
        endpoint.parameters = query_parameters(parsed.query);
    } catch (...) {
        endpoint.path = "/";
    }
    endpoint.notes.push_back("observed response status " +
                             std::to_string(status));
    observed_.push_back(std::move(endpoint));
}

EndpointExtractionResult EndpointExtractor::extract(
    const Document& document) const {
    EndpointExtractionResult result;
    std::vector<DiscoveredEndpoint> found;
    std::map<std::string, std::size_t> seen;

    const auto add = [&](DiscoveredEndpoint endpoint) {
        const std::string key = endpoint.method + " " + endpoint.path;
        const auto duplicate = seen.find(key);
        if (duplicate != seen.end()) {
            auto& existing = found[duplicate->second];
            append_unique_all(existing.parameters, endpoint.parameters);
            append_unique_all(existing.notes, endpoint.notes);
            if (existing.request_content_type.empty()) {
                existing.request_content_type = endpoint.request_content_type;
            }
            if (existing.response_content_type.empty()) {
                existing.response_content_type = endpoint.response_content_type;
            }
            if (endpoint.confidence > existing.confidence) {
                existing.url = std::move(endpoint.url);
                existing.source = std::move(endpoint.source);
                existing.discovery_method = std::move(endpoint.discovery_method);
                existing.confidence = endpoint.confidence;
            }
            return;
        }
        seen[key] = found.size();
        found.push_back(std::move(endpoint));
    };

    if (options_.observe_network) {
        for (const auto& endpoint : observed_) {
            add(endpoint);
        }
    }

    const std::string base = document.base_url();

    const auto resolve = [&base](const std::string& reference) -> std::string {
        if (reference.empty()) {
            return reference;
        }
        try {
            return base.empty() ? normalize_url(reference)
                                : resolve_url(base, reference);
        } catch (...) {
            return reference;
        }
    };

    if (options_.follow_links) {
        for (const auto& link : document.links()) {
            const std::string href = link->attribute("href");
            if (href.empty() || href[0] == '#') {
                continue;
            }
            const std::string url = resolve(href);
            Url parsed;
            try {
                parsed = parse_url(url);
            } catch (...) {
                continue;
            }
            if (!looks_like_api_path(parsed.path)) {
                continue;
            }
            DiscoveredEndpoint endpoint;
            endpoint.url = url;
            endpoint.path = parsed.path;
            endpoint.method = "get";
            endpoint.source = document.url();
            endpoint.discovery_method = "html-link";
            endpoint.confidence = 0.40;
            endpoint.parameters = query_parameters(parsed.query);
            endpoint.notes.push_back("inferred from an anchor href");
            add(std::move(endpoint));
        }
    }

    for (const auto& form : document.forms()) {
        const std::string action = form->attribute("action");
        const std::string url = resolve(action.empty() ? document.url() : action);
        Url parsed;
        try {
            parsed = parse_url(url);
        } catch (...) {
            continue;
        }
        DiscoveredEndpoint endpoint;
        endpoint.url = url;
        endpoint.path = parsed.path.empty() ? "/" : parsed.path;
        endpoint.method = method_from_form(*form);
        endpoint.source = document.url();
        endpoint.discovery_method = "html-form";
        endpoint.confidence = endpoint.method == "get" ? 0.55 : 0.75;
        endpoint.parameters = form_parameters(*form);
        append_unique_all(endpoint.parameters, query_parameters(parsed.query));
        endpoint.request_content_type =
            endpoint.method == "get" ? "" : "application/x-www-form-urlencoded";
        endpoint.notes.push_back("inferred from a form action");
        add(std::move(endpoint));
    }

    if (options_.inspect_scripts) {
        for (const auto& script : document.scripts()) {
            if (script->has_attribute("src")) {
                continue;
            }
            for (const auto& [method, reference] :
                 script_endpoints(script->text())) {
                if (reference.empty()) {
                    continue;
                }
                const std::string url = resolve(reference);
                Url parsed;
                try {
                    parsed = parse_url(url);
                } catch (...) {
                    continue;
                }
                DiscoveredEndpoint endpoint;
                endpoint.url = url;
                endpoint.path = parsed.path.empty() ? "/" : parsed.path;
                endpoint.method = normalize_method(method);
                endpoint.source = document.url();
                endpoint.discovery_method = "inline-script";
                endpoint.confidence = 0.65;
                endpoint.parameters = query_parameters(parsed.query);
                endpoint.notes.push_back(
                    "inferred from a fetch/XMLHttpRequest call");
                add(std::move(endpoint));
            }
        }
    }

    result.endpoints.reserve(found.size());
    for (auto& endpoint : found) {
        if (endpoint.confidence < options_.minimum_confidence) {
            result.warnings.push_back("skipped low-confidence endpoint " +
                                      endpoint.path + " (" +
                                      format_confidence(endpoint.confidence) +
                                      ")");
            continue;
        }
        std::sort(endpoint.parameters.begin(), endpoint.parameters.end());
        result.endpoints.push_back(std::move(endpoint));
    }

    std::sort(result.endpoints.begin(), result.endpoints.end(),
              [](const DiscoveredEndpoint& a, const DiscoveredEndpoint& b) {
                  if (a.path != b.path) {
                      return a.path < b.path;
                  }
                  return a.method < b.method;
              });

    const Redactor redactor;
    result.openapi_yaml =
        render_openapi_yaml(result.endpoints, options_, redactor);
    return result;
}

std::string render_openapi_yaml(
    const std::vector<DiscoveredEndpoint>& endpoints,
    const EndpointExtractionOptions& options, const Redactor& redactor) {
    std::ostringstream out;
    out << "openapi: " << options.openapi_version << "\n";
    out << "info:\n";
    out << "  title: 'Discovered API'\n";
    out << "  version: '0.1.0'\n";
    out << "  description: >-\n";
    out << "    Generated by ProwseTk endpoint extraction. Endpoints are\n";
    out << "    heuristic discoveries, not authoritative API documentation.\n";
    out << "    Inferred values are marked and carry provenance and\n";
    out << "    confidence.\n";
    out << "x-prowsetk-generated: true\n";

    std::map<std::string, std::vector<const DiscoveredEndpoint*>> by_path;
    for (const auto& endpoint : endpoints) {
        by_path[endpoint.path].push_back(&endpoint);
    }

    out << "paths:";
    if (by_path.empty()) {
        out << " {}\n";
        return out.str();
    }
    out << "\n";
    for (const auto& [path, entries] : by_path) {
        out << "  " << yaml_quote(path) << ":\n";
        for (const auto* endpoint : entries) {
            out << "    " << endpoint->method << ":\n";
            out << "      operationId: "
                << operation_id(endpoint->method, endpoint->path) << "\n";
            if (!endpoint->parameters.empty()) {
                out << "      parameters:\n";
                for (const auto& parameter : endpoint->parameters) {
                    out << "        - name: " << yaml_quote(parameter) << "\n";
                    out << "          in: query\n";
                    out << "          required: false\n";
                    out << "          schema:\n";
                    out << "            type: string\n";
                }
            }
            if (!endpoint->request_content_type.empty()) {
                out << "      requestBody:\n";
                out << "        content:\n";
                out << "          " << yaml_quote(endpoint->request_content_type)
                    << ":\n";
                out << "            schema:\n";
                out << "              type: object\n";
                out << "              x-inferred: true\n";
            }
            if (!endpoint->response_content_type.empty()) {
                out << "      responses:\n";
                out << "        '200':\n";
                out << "          description: Observed response\n";
                out << "          content:\n";
                out << "            " << yaml_quote(endpoint->response_content_type)
                    << ":\n";
                out << "              schema:\n";
                out << "                type: object\n";
                out << "                x-inferred: true\n";
            } else {
                out << "      responses:\n";
                out << "        '200':\n";
                out << "          description: Inferred response\n";
            }
            if (options.infer_schemas) {
                out << "      x-inferred: true\n";
            }
            if (options.include_provenance) {
                const std::string source = options.redact_secrets
                                               ? redactor.redact_url(endpoint->source)
                                               : endpoint->source;
                const std::string endpoint_url =
                    options.redact_secrets
                        ? redactor.redact_url(endpoint->url)
                        : endpoint->url;
                out << "      x-prowsetk-provenance:\n";
                out << "        source: " << yaml_quote(source) << "\n";
                out << "        url: " << yaml_quote(endpoint_url) << "\n";
                out << "        discovery-method: "
                    << yaml_quote(endpoint->discovery_method) << "\n";
                out << "        confidence: "
                    << format_confidence(endpoint->confidence) << "\n";
                out << "        inferred: true\n";
                if (!endpoint->notes.empty()) {
                    out << "        notes:\n";
                    for (const auto& note : endpoint->notes) {
                        out << "          - " << yaml_quote(note) << "\n";
                    }
                }
            }
        }
    }
    return out.str();
}

}  // namespace prowsetk
