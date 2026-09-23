#include "prowsetk/plugins/restful_resolver.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <regex>
#include <sstream>

#include "prowsetk/url.hpp"

namespace prowsetk::plugins::restful_resolver {
namespace {

std::string to_lower(std::string v) {
    std::transform(v.begin(), v.end(), v.begin(), [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    });
    return v;
}

bool path_contains(const std::string& path, const std::string& pattern) {
    if (pattern.empty()) return false;
    return to_lower(path).find(to_lower(pattern)) != std::string::npos;
}

bool builtin_api_marker(const std::string& path) {
    static const char* const markers[] = {"/api", "/v1", "/v2", "/v3",
                                          "/graphql", "/rest", "/rpc",
                                          "/json", ".json", "/data",
                                          "/internal", "/private",
                                          "/hotel/hoteladmin",
                                          "/partner-settings"};
    const std::string lower = to_lower(path);
    for (const char* m : markers) {
        if (lower.find(m) != std::string::npos) return true;
    }
    return false;
}

std::string yaml_quote(std::string_view v) {
    std::string r = "'";
    for (char c : v) {
        if (c == '\'')
            r += "''";
        else if (c == '\n')
            r += "\\n";
        else
            r.push_back(c);
    }
    r += "'";
    return r;
}

std::string operation_id(const std::string& method, const std::string& path) {
    std::string id = to_lower(method) + "_" + path;
    for (char& c : id)
        if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
    while (id.find("__") != std::string::npos) id.replace(id.find("__"), 2, "_");
    if (!id.empty() && id.front() == '_') id.erase(id.begin());
    if (!id.empty() && id.back() == '_') id.pop_back();
    if (id.empty()) id = "op";
    return id;
}

std::string format_conf(double c) {
    std::ostringstream s;
    s.setf(std::ios::fixed);
    s.precision(2);
    s << c;
    return s.str();
}

std::string origin_of(const std::string& url) {
    try {
        Url parsed = parse_url(url);
        if (!parsed.has_host()) return {};
        std::string origin = to_lower(parsed.scheme) + "://" + to_lower(parsed.host);
        if (!parsed.port.empty()) origin += ":" + parsed.port;
        return origin;
    } catch (...) {
        return {};
    }
}

bool same_origin(const std::string& a, const std::string& b) {
    const std::string oa = origin_of(a);
    const std::string ob = origin_of(b);
    return !oa.empty() && oa == ob;
}

EndpointExtractionOptions to_extraction_opts(const RestfulResolverOptions& o) {
    EndpointExtractionOptions e;
    e.follow_links = true;
    e.inspect_scripts = o.inspect_scripts;
    e.observe_network = false;
    e.infer_schemas = o.infer_schemas;
    e.include_provenance = o.include_provenance;
    e.redact_secrets = o.redact_secrets;
    e.max_depth = 2;
    e.max_pages = 100;
    e.minimum_confidence = o.minimum_confidence;
    e.openapi_version = o.openapi_version;
    e.scrape_all_paths = false;
    return e;
}

// Light JSON/JS API-link extractor: scans quoted strings that look like API
// paths or absolute URLs containing an API pattern. Heuristic and
// deliberately conservative.
std::vector<std::string> extract_api_urls_from_body(
    const std::string& body, const std::vector<std::string>& patterns) {
    std::vector<std::string> out;
    if (body.empty()) return out;
    static const std::regex quoted(R"((\"([^\"\\]{1,2048})\"|'([^'\\]{1,2048})'))");
    std::sregex_iterator it(body.begin(), body.end(), quoted);
    std::sregex_iterator end;
    for (; it != end; ++it) {
        std::string candidate;
        if ((*it)[2].matched)
            candidate = (*it)[2].str();
        else if ((*it)[3].matched)
            candidate = (*it)[3].str();
        if (candidate.empty()) continue;
        bool is_api = false;
        for (const auto& pat : patterns) {
            if (path_contains(candidate, pat)) {
                is_api = true;
                break;
            }
        }
        if (!is_api && !builtin_api_marker(candidate)) continue;
        if (candidate.rfind("/", 0) == 0 || candidate.rfind("http://", 0) == 0 ||
            candidate.rfind("https://", 0) == 0) {
            if (std::find(out.begin(), out.end(), candidate) == out.end()) {
                out.push_back(candidate);
                if (out.size() >= 32) break;
            }
        }
    }
    return out;
}

// Runs the core EndpointExtractor over an HTML response body so POST forms
// and fetch/XHR POST calls inside resolved pages are discovered with their
// native methods and provenance preserved.
std::vector<DiscoveredEndpoint> extract_from_html_body(
    const std::string& body, const std::string& final_url,
    const RestfulResolverOptions& options) {
    std::vector<DiscoveredEndpoint> out;
    if (body.empty()) return out;
    const std::string lower_ct = to_lower(body.substr(0, 512));
    const bool looks_html = body.find('<') != std::string::npos &&
                            (body.find("<html") != std::string::npos ||
                             body.find("<form") != std::string::npos ||
                             body.find("<script") != std::string::npos ||
                             body.find("<a ") != std::string::npos ||
                             lower_ct.find("<!doctype") != std::string::npos);
    if (!looks_html) return out;
    auto document = parse_html(body, final_url, final_url);
    if (document == nullptr) return out;
    EndpointExtractor extractor(to_extraction_opts(options));
    EndpointExtractionResult raw = extractor.extract(*document);
    for (auto& e : raw.endpoints) {
        if (e.source.empty()) e.source = final_url;
        bool noted = false;
        for (const auto& n : e.notes) {
            if (n.find("resolved page") != std::string::npos) {
                noted = true;
                break;
            }
        }
        if (!noted) e.notes.push_back("discovered in resolved page body");
        out.push_back(std::move(e));
    }
    return out;
}

void refresh_completeness(RestfulResolverResult& result) {
    result.has_get = has_get_endpoint(result.endpoints);
    result.has_post = has_post_endpoint(result.endpoints);
    result.is_complete = result.has_get && result.has_post;
}

}  // namespace

bool is_restful_api_path(const std::string& path,
                         const std::vector<std::string>& patterns) {
    if (patterns.empty()) return builtin_api_marker(path);
    for (const auto& pat : patterns) {
        if (path_contains(path, pat)) return true;
    }
    return builtin_api_marker(path);
}

std::vector<DiscoveredEndpoint> filter_to_api(
    const std::vector<DiscoveredEndpoint>& endpoints,
    const RestfulResolverOptions& options) {
    if (!options.require_api_pattern) return endpoints;
    std::vector<DiscoveredEndpoint> out;
    out.reserve(endpoints.size());
    for (const auto& e : endpoints) {
        if (is_restful_api_path(e.path, options.api_patterns)) out.push_back(e);
    }
    return out;
}

bool has_post_endpoint(const std::vector<DiscoveredEndpoint>& endpoints) {
    for (const auto& e : endpoints) {
        if (to_lower(e.method) == "post") return true;
    }
    return false;
}

bool has_get_endpoint(const std::vector<DiscoveredEndpoint>& endpoints) {
    for (const auto& e : endpoints) {
        if (to_lower(e.method) == "get") return true;
    }
    return false;
}

RestfulResolverResult resolve_from_document(
    const Document& document, const RestfulResolverOptions& options,
    const Redactor& redactor) {
    EndpointExtractor extractor(to_extraction_opts(options));
    EndpointExtractionResult raw = extractor.extract(document);

    RestfulResolverResult result;
    result.endpoints = filter_to_api(raw.endpoints, options);
    result.warnings = raw.warnings;
    result.resolved.reserve(result.endpoints.size());
    for (auto& e : result.endpoints) {
        ResolvedCall r;
        r.endpoint = e;
        r.final_url = e.url;
        r.round = 0;
        result.resolved.push_back(std::move(r));
    }
    // De-duplicate so completeness reflects unique method+path pairs.
    {
        std::map<std::string, DiscoveredEndpoint> dedup;
        for (auto& r : result.resolved) {
            const std::string key = r.endpoint.method + " " + r.endpoint.path;
            if (dedup.find(key) == dedup.end()) dedup[key] = r.endpoint;
        }
        result.endpoints.clear();
        for (auto& [key, ep] : dedup) result.endpoints.push_back(ep);
        std::sort(result.endpoints.begin(), result.endpoints.end(),
                  [](const auto& a, const auto& b) {
                      if (a.path != b.path) return a.path < b.path;
                      return a.method < b.method;
                  });
    }
    refresh_completeness(result);
    if (!result.is_complete) {
        result.warnings.push_back(
            "seed document does not expose a full RESTful surface (GET+POST); "
            "resolve through a session to probe scraped endpoints");
    }
    result.openapi_yaml = render_resolver_yaml(result.resolved, result, options, redactor);
    return result;
}

RestfulResolverResult resolve_from_session(Session& session,
                                           const RestfulResolverOptions& options) {
    const Redactor redactor;
    auto document = session.document();
    if (document == nullptr) {
        RestfulResolverResult empty;
        empty.warnings.push_back("session has no document");
        empty.openapi_yaml = render_resolver_yaml({}, empty, options, redactor);
        return empty;
    }

    EndpointExtractor extractor(to_extraction_opts(options));
    EndpointExtractionResult raw = extractor.extract(*document);
    auto seeds = filter_to_api(raw.endpoints, options);

    RestfulResolverResult result;
    result.warnings = raw.warnings;

    struct QueueItem {
        DiscoveredEndpoint ep;
        std::uint32_t round;
    };
    std::vector<QueueItem> queue;
    // Seed calls are recorded even when no fetch happens yet.
    std::map<std::string, size_t> visited;  // url -> resolved index
    std::map<std::string, bool> queued;
    // Every known endpoint (seeds plus anything discovered mid-resolution),
    // keyed by method+path. `result.endpoints` is rebuilt from this union so
    // a POST discovered for an already-fetched URL still counts.
    std::map<std::string, DiscoveredEndpoint> known;

    std::string base = document->base_url().empty() ? document->url() : document->base_url();
    if (base.empty()) base = session.current_url();

    const std::string seed_origin = origin_of(base);
    std::uint32_t fetches = 0;
    std::uint32_t max_round = options.max_rounds;

    auto absolute = [&](const std::string& url) -> std::string {
        try {
            if (!base.empty() && url.rfind("http://", 0) != 0 &&
                url.rfind("https://", 0) != 0) {
                return resolve_url(base, url);
            }
        } catch (...) {
        }
        return url;
    };

    auto origin_allowed = [&](const std::string& url) {
        return options.allow_cross_origin || seed_origin.empty() ||
               same_origin(base, url);
    };

    auto note_discovered = [&](const DiscoveredEndpoint& ep) {
        const std::string key = ep.method + " " + ep.path;
        auto it = known.find(key);
        if (it == known.end() || ep.confidence > it->second.confidence) {
            known[key] = ep;
        }
    };

    // Normalize seeds to absolute URLs and drop out-of-scope ones upfront.
    for (auto& s : seeds) {
        DiscoveredEndpoint ep = s;
        ep.url = absolute(ep.url);
        if (!origin_allowed(ep.url)) continue;
        note_discovered(ep);
        if (queued.find(ep.url) == queued.end()) {
            queued[ep.url] = true;
            queue.push_back({ep, 0});
        }
    }

    auto enqueue = [&](DiscoveredEndpoint ep, std::uint32_t round) {
        if (round > max_round) return;
        ep.url = absolute(ep.url);
        if (!origin_allowed(ep.url)) return;
        if (options.require_api_pattern &&
            !is_restful_api_path(ep.path, options.api_patterns)) {
            return;
        }
        note_discovered(ep);
        if (visited.find(ep.url) != visited.end() ||
            queued.find(ep.url) != queued.end()) {
            return;
        }
        queued[ep.url] = true;
        queue.push_back({std::move(ep), round});
    };

    auto rebuild_endpoints = [&]() {
        for (auto& r : result.resolved) note_discovered(r.endpoint);
        result.endpoints.clear();
        result.endpoints.reserve(known.size());
        for (auto& [key, ep] : known) result.endpoints.push_back(ep);
        std::sort(result.endpoints.begin(), result.endpoints.end(),
                  [](const auto& a, const auto& b) {
                      if (a.path != b.path) return a.path < b.path;
                      return a.method < b.method;
                  });
        refresh_completeness(result);
    };

    while (!queue.empty() && fetches < options.max_requests) {
        QueueItem item = queue.front();
        queue.erase(queue.begin());
        if (item.round > max_round) continue;

        std::string fetch_url = item.ep.url;
        try {
            if (!base.empty() && fetch_url.rfind("http://", 0) != 0 &&
                fetch_url.rfind("https://", 0) != 0) {
                fetch_url = resolve_url(base, fetch_url);
            }
        } catch (...) {
        }
        if (visited.find(fetch_url) != visited.end()) continue;
        if (!options.allow_cross_origin && !seed_origin.empty() &&
            !same_origin(base, fetch_url)) {
            continue;
        }

        ResolvedCall call;
        call.endpoint = item.ep;
        call.endpoint.url = fetch_url;
        call.final_url = fetch_url;
        call.round = item.round;
        visited[fetch_url] = result.resolved.size();

        try {
            HttpRequest req;
            req.method = "GET";
            req.url = fetch_url;
            HttpResponse resp = session.request(req);
            call.status = resp.status;
            call.final_url =
                resp.final_url.empty() ? fetch_url : resp.final_url;
            call.redirect_chain = resp.redirect_chain;
            auto ct = resp.header("Content-Type");
            call.content_type = ct;
            if (!ct.empty()) call.endpoint.response_content_type = ct;
            fetches++;
            result.rounds_used = std::max(result.rounds_used, item.round);

            const std::string body = resp.body;
            const std::string lower_ct = to_lower(ct);
            const bool looks_json =
                lower_ct.find("json") != std::string::npos ||
                lower_ct.find("javascript") != std::string::npos ||
                (!body.empty() && (body.front() == '{' || body.front() == '['));
            if (options.follow_json_links &&
                (looks_json || lower_ct.find("text/") != std::string::npos ||
                 lower_ct.find("html") != std::string::npos || ct.empty())) {
                for (auto& du :
                     extract_api_urls_from_body(body, options.api_patterns)) {
                    std::string u = du;
                    try {
                        if (u.rfind("http", 0) != 0) u = resolve_url(call.final_url, u);
                        Url parsed = parse_url(u);
                        std::string path =
                            parsed.path.empty() ? "/" : parsed.path;
                        if (options.require_api_pattern &&
                            !is_restful_api_path(path, options.api_patterns))
                            continue;
                        DiscoveredEndpoint ne;
                        ne.url = u;
                        ne.path = path;
                        ne.method = "get";
                        ne.source = call.final_url;
                        ne.discovery_method = "resolved-json";
                        ne.confidence = 0.60;
                        ne.notes.push_back(
                            "heuristically discovered in a resolved response body");
                        enqueue(std::move(ne), item.round + 1);
                    } catch (...) {
                    }
                }
            }
            const bool looks_html =
                lower_ct.find("html") != std::string::npos ||
                (!body.empty() && body.find('<') != std::string::npos &&
                 (body.find("<form") != std::string::npos ||
                  body.find("<script") != std::string::npos));
            if (looks_html) {
                for (auto& ne :
                     extract_from_html_body(body, call.final_url, options)) {
                    if (options.require_api_pattern &&
                        !is_restful_api_path(ne.path, options.api_patterns))
                        continue;
                    enqueue(ne, item.round + 1);
                }
            }
        } catch (const std::exception& ex) {
            call.error = ex.what();
            fetches++;
        } catch (...) {
            call.error = "unknown error during resolution";
            fetches++;
        }
        result.resolved.push_back(std::move(call));

        // Rebuild the union (fetched endpoints plus anything discovered but
        // not yet fetched) and stop once GET+POST are both known.
        rebuild_endpoints();
        if (result.is_complete) break;
    }

    // Carry over any known endpoints that were never resolved due to budget
    // limits so the YAML still reports the full discovered surface.
    for (auto& [key, ep] : known) {
        bool already = false;
        for (auto& r : result.resolved) {
            if (r.endpoint.method + " " + r.endpoint.path == key) {
                already = true;
                break;
            }
        }
        if (!already) {
            ResolvedCall call;
            call.endpoint = ep;
            call.final_url = ep.url;
            call.round = 0;
            call.error = fetches >= options.max_requests
                             ? "resolution budget exhausted"
                             : "not resolved (round limit)";
            result.resolved.push_back(std::move(call));
        }
    }

    rebuild_endpoints();
    result.request_count = fetches;
    if (!result.is_complete) {
        std::ostringstream note;
        note << "resolution stopped after " << fetches << " request(s) across "
             << result.rounds_used << " round(s) without discovering a full "
             << "RESTful surface (GET+POST); coverage is heuristic and incomplete";
        result.warnings.push_back(note.str());
    }
    result.openapi_yaml =
        render_resolver_yaml(result.resolved, result, options, redactor);
    return result;
}

RestfulResolverResult resolve(Session& session,
                              const RestfulResolverOptions& options) {
    if (!options.html.empty()) {
        std::string base = options.base_url.empty() ? options.url : options.base_url;
        if (base.empty()) base = "https://example.com/";
        session.load_html(options.html, base);
        return resolve_from_session(session, options);
    }
    if (!options.url.empty()) {
        try {
            session.navigate(options.url);
        } catch (...) {
            if (session.document() == nullptr) throw;
        }
    }
    return resolve_from_session(session, options);
}

std::string render_resolver_yaml(const std::vector<ResolvedCall>& resolved,
                                 const RestfulResolverResult& summary,
                                 const RestfulResolverOptions& options,
                                 const Redactor& redactor) {
    std::ostringstream out;
    out << "openapi: " << options.openapi_version << "\n";
    out << "info:\n";
    out << "  title: 'Discovered API (restful-resolver)'\n";
    out << "  version: '0.1.0'\n";
    out << "  description: >-\n";
    out << "    Generated by ProwseTk restful-resolver. Endpoints are\n";
    out << "    heuristic discoveries resolved iteratively until a full\n";
    out << "    RESTful surface (GET+POST) is found; not authoritative API\n";
    out << "    documentation. Inferred values are marked and carry\n";
    out << "    provenance and confidence.\n";
    out << "x-prowsetk-generated: true\n";
    out << "x-prowsetk-plugin: restful-resolver\n";
    out << "x-prowsetk-restful:\n";
    out << "  has-get: " << (summary.has_get ? "true" : "false") << "\n";
    out << "  has-post: " << (summary.has_post ? "true" : "false") << "\n";
    out << "  is-complete: " << (summary.is_complete ? "true" : "false") << "\n";
    out << "  rounds-used: " << summary.rounds_used << "\n";
    out << "  request-count: " << summary.request_count << "\n";
    out << "  note: 'Heuristic iterative resolution; not authoritative.'\n";

    std::map<std::string, std::vector<const ResolvedCall*>> by_path;
    for (const auto& r : resolved) {
        std::string p = r.endpoint.path.empty() ? "/" : r.endpoint.path;
        by_path[p].push_back(&r);
    }
    out << "paths:";
    if (by_path.empty()) {
        out << " {}\n";
        return out.str();
    }
    out << "\n";
    for (const auto& [path, entries] : by_path) {
        out << "  " << yaml_quote(path) << ":\n";
        // Deterministic method order.
        std::vector<const ResolvedCall*> sorted = entries;
        std::sort(sorted.begin(), sorted.end(), [](const auto* a, const auto* b) {
            return a->endpoint.method < b->endpoint.method;
        });
        for (const auto* rc : sorted) {
            const auto& ep = rc->endpoint;
            out << "    " << ep.method << ":\n";
            out << "      operationId: " << operation_id(ep.method, ep.path) << "\n";
            if (!ep.parameters.empty()) {
                out << "      parameters:\n";
                for (const auto& param : ep.parameters) {
                    out << "        - name: " << yaml_quote(param) << "\n";
                    out << "          in: query\n";
                    out << "          required: false\n";
                    out << "          schema:\n";
                    out << "            type: string\n";
                }
            }
            if (!ep.request_content_type.empty()) {
                out << "      requestBody:\n";
                out << "        content:\n";
                out << "          " << yaml_quote(ep.request_content_type) << ":\n";
                out << "            schema:\n";
                out << "              type: object\n";
                out << "              x-inferred: true\n";
            }
            std::string ct =
                rc->content_type.empty() ? ep.response_content_type : rc->content_type;
            if (!ct.empty()) {
                out << "      responses:\n";
                out << "        '" << (rc->status ? std::to_string(rc->status) : "200")
                    << "':\n";
                out << "          description: "
                    << (rc->status ? "Observed response" : "Inferred response") << "\n";
                out << "          content:\n";
                out << "            " << yaml_quote(ct) << ":\n";
                out << "              schema:\n";
                out << "                type: object\n";
                out << "                x-inferred: true\n";
            } else {
                out << "      responses:\n";
                out << "        '" << (rc->status ? std::to_string(rc->status) : "200")
                    << "':\n";
                out << "          description: "
                    << (rc->status ? "Observed response" : "Inferred response") << "\n";
            }
            if (options.infer_schemas) out << "      x-inferred: true\n";
            if (options.include_provenance) {
                std::string source = options.redact_secrets
                                         ? redactor.redact_url(ep.source)
                                         : ep.source;
                std::string url = options.redact_secrets
                                      ? redactor.redact_url(ep.url)
                                      : ep.url;
                std::string final_url = options.redact_secrets
                                            ? redactor.redact_url(rc->final_url)
                                            : rc->final_url;
                out << "      x-prowsetk-provenance:\n";
                out << "        source: " << yaml_quote(source) << "\n";
                out << "        url: " << yaml_quote(url) << "\n";
                if (!final_url.empty() && final_url != url) {
                    out << "        final-url: " << yaml_quote(final_url) << "\n";
                }
                out << "        discovery-method: " << yaml_quote(ep.discovery_method)
                    << "\n";
                out << "        confidence: " << format_conf(ep.confidence) << "\n";
                out << "        inferred: true\n";
                out << "        round: " << rc->round << "\n";
                if (!rc->redirect_chain.empty()) {
                    out << "        redirect-chain:\n";
                    for (const auto& u : rc->redirect_chain) {
                        std::string ru = options.redact_secrets
                                             ? redactor.redact_url(u)
                                             : u;
                        out << "          - " << yaml_quote(ru) << "\n";
                    }
                }
                if (rc->status != 0) out << "        http-status: " << rc->status << "\n";
                if (!rc->error.empty()) {
                    out << "        resolve-error: " << yaml_quote(rc->error) << "\n";
                }
                if (!ep.notes.empty()) {
                    out << "        notes:\n";
                    for (const auto& n : ep.notes)
                        out << "          - " << yaml_quote(n) << "\n";
                }
            }
        }
    }
    return out.str();
}

}  // namespace prowsetk::plugins::restful_resolver
