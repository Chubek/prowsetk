#include "prowsetk/plugins/scrape_endpoints.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <locale>
#include <map>
#include <regex>
#include <sstream>

#include "prowsetk/url.hpp"
#include "prowsetk/browser.hpp"

namespace prowsetk::plugins::scrape_endpoints {
namespace {

std::string to_lower(std::string v) {
    std::transform(v.begin(), v.end(), v.begin(), [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    });
    return v;
}

bool path_contains(const std::string& path, const std::string& pattern) {
    if (pattern.empty()) return false;
    std::string lower_path = to_lower(path);
    std::string lower_pat = to_lower(pattern);
    return lower_path.find(lower_pat) != std::string::npos;
}

// Built-in API markers used by endpoint_extraction.cpp plus scrape-endpoints extras.
// Beacon/challenge markers keep telemetry-style POST endpoints (e.g.
// `/__challenge_.../telemetry`, `navigator.sendBeacon` targets) API-like.
bool builtin_api_marker(const std::string& path) {
    static const char* const markers[] = {"/api", "/v1", "/v2", "/v3",
                                          "/graphql", "/rest", "/rpc",
                                          "/json", ".json", "/data",
                                          "/ajax", "/gateway", "/service",
                                          "/backend", "/bff", "/dml",
                                          "/internal", "/private",
                                          "/hotel/hoteladmin",
                                          "/partner-settings",
                                          "/telemetry", "challenge",
                                          "/beacon", "/collect"};
    std::string lower = to_lower(path);
    for (const char* m : markers) {
        if (lower.find(m) != std::string::npos) return true;
    }
    return false;
}

std::string yaml_quote(std::string_view v) {
    std::string r = "'";
    for (char c : v) {
        if (c == '\'') r += "''";
        else if (c == '\n') r += "\\n";
        else r.push_back(c);
    }
    r += "'";
    return r;
}

std::string operation_id(const std::string& method, const std::string& path) {
    std::string id = to_lower(method) + "_" + path;
    for (char& c : id) if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
    while (id.find("__") != std::string::npos) id.replace(id.find("__"), 2, "_");
    if (!id.empty() && id.front() == '_') id.erase(id.begin());
    if (!id.empty() && id.back() == '_') id.pop_back();
    if (id.empty()) id = "op";
    return id;
}

std::string format_conf(double c) {
    std::ostringstream s;
    s.setf(std::ios::fixed); s.precision(2); s << c;
    return s.str();
}

// Very light JSON API link extractor. Not a JSON parser; scans for quoted
// strings that look like API paths or absolute URLs containing an API pattern.
// This is heuristic and deliberately conservative.
std::vector<std::string> extract_api_urls_from_body(
    const std::string& body, const std::vector<std::string>& patterns) {
    std::vector<std::string> out;
    if (body.empty()) return out;
    // Match quoted strings: "..."  and '...'  up to 2048 chars
    static const std::regex quoted(R"((\"([^\"\\]{1,2048})\"|'([^'\\]{1,2048})'))");
    std::sregex_iterator it(body.begin(), body.end(), quoted);
    std::sregex_iterator end;
    for (; it != end; ++it) {
        std::string candidate;
        if ((*it)[2].matched) candidate = (*it)[2].str();
        else if ((*it)[3].matched) candidate = (*it)[3].str();
        if (candidate.empty()) continue;
        // Must contain an API pattern and look like a path or URL
        bool is_api = false;
        for (const auto& pat : patterns) {
            if (path_contains(candidate, pat)) { is_api = true; break; }
        }
        if (!is_api && !builtin_api_marker(candidate)) continue;
        // Accept only relative paths (/api/...) or http(s) URLs
        if (candidate.rfind("/", 0) == 0 || candidate.rfind("http://", 0) == 0 ||
            candidate.rfind("https://", 0) == 0) {
            // dedupe
            if (std::find(out.begin(), out.end(), candidate) == out.end()) {
                out.push_back(candidate);
                if (out.size() >= 32) break;
            }
        }
    }
    return out;
}

std::string assistant_command(const ScrapeEndpointsOptions& options) {
    if (!options.assistant_browser.empty()) return options.assistant_browser;
    return "assistant-browser";
}

std::string assistant_url(const Session& session) {
    if (const auto document = session.document()) {
        if (!document->url().empty()) return document->url();
        if (!document->base_url().empty()) return document->base_url();
    }
    return session.current_url();
}

EndpointExtractionOptions to_extraction_opts(const ScrapeEndpointsOptions& o) {
    EndpointExtractionOptions e;
    e.follow_links = o.follow_links;
    e.inspect_scripts = o.inspect_scripts;
    e.observe_network = o.observe_network;
    e.infer_schemas = o.infer_schemas;
    e.include_provenance = o.include_provenance;
    e.redact_secrets = o.redact_secrets;
    e.max_depth = o.max_depth;
    e.max_pages = o.max_pages;
    e.minimum_confidence = o.minimum_confidence;
    e.openapi_version = o.openapi_version;
    return e;
}

}  // namespace

bool is_api_path(const std::string& path,
                 const std::vector<std::string>& patterns) {
    if (patterns.empty()) return builtin_api_marker(path);
    for (const auto& pat : patterns) {
        if (path_contains(path, pat)) return true;
    }
    return builtin_api_marker(path);
}

const std::vector<std::string>& default_garbage_patterns() {
    static const std::vector<std::string> patterns = {
        ".js",   ".mjs",  ".cjs",  ".jsx",  ".ts",   ".tsx",  ".css",
        ".less", ".scss", ".sass", ".map",  ".png",  ".jpg",  ".jpeg",
        ".gif",  ".svg",  ".ico",  ".webp", ".avif", ".bmp",  ".tif",
        ".tiff", ".woff", ".woff2", ".ttf", ".otf",  ".eot",  ".mp4",
        ".webm", ".ogv",  ".mp3",  ".wav",  ".ogg",  ".flac", ".avi",
        ".mov",  ".pdf",  ".zip",  ".gz",   ".tar",  ".rar",  ".7z",
        ".dmg",  ".exe",  ".msi",  ".swf",  ".flv",  "/static/", "/assets/",
        "/fonts/", "/font/",  "/images/", "/image/", "/img/",   "/css/",
        "/js/",    "/icons/", "/icon/",     "/logos/", "/media/", "/videos/",
        "/thumbnails/", "/node_modules/", "/favicon", ".well-known/"};
    return patterns;
}

// Strips any query string or fragment so "?v=1.2.js" cannot smuggle an asset
// suffix past the matcher and "/page?x=.js" cannot fake one.
std::string strip_query_fragment(const std::string& path) {
    const auto end = path.find_first_of("?#");
    return end == std::string::npos ? path : path.substr(0, end);
}

bool is_garbage_path(const std::string& path,
                     const std::vector<std::string>& patterns) {
    // Mirroring is_api_path: an empty pattern list selects the defaults.
    const std::vector<std::string>& effective =
        patterns.empty() ? default_garbage_patterns() : patterns;
    const std::string bare = to_lower(strip_query_fragment(path));
    if (bare.empty()) return false;
    for (const auto& pat : effective) {
        if (pat.empty()) continue;
        const std::string lower_pat = to_lower(pat);
        if (!lower_pat.empty() && lower_pat.front() == '.' &&
            lower_pat.find('/') == std::string::npos) {
            // Asset suffix: must match at the very end of the path.
            if (bare.size() >= lower_pat.size() &&
                bare.compare(bare.size() - lower_pat.size(), lower_pat.size(),
                             lower_pat) == 0) {
                return true;
            }
        } else if (bare.find(lower_pat) != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::vector<DiscoveredEndpoint> filter_to_api(
    const std::vector<DiscoveredEndpoint>& endpoints,
    const ScrapeEndpointsOptions& options) {
    if (!options.api_only) return endpoints;
    std::vector<DiscoveredEndpoint> out;
    out.reserve(endpoints.size());
    for (const auto& e : endpoints) {
        // Garbage first: a static asset is gunk even when its path carries
        // an API marker or its method is not GET.
        if (is_garbage_path(e.path, options.garbage_patterns)) {
            continue;
        }
        if (!options.require_api_pattern) {
            out.push_back(e);
            continue;
        }
        if (is_api_path(e.path, options.api_patterns)) {
            out.push_back(e);
            continue;
        }
        // An explicit non-GET method (POST form, fetch/XHR POST, beacon) is
        // itself evidence of a backend surface, so it is kept even when the
        // path carries no API marker. Discovery stays heuristic: provenance
        // and confidence still mark the endpoint as inferred.
        if (to_lower(e.method) != "get") {
            out.push_back(e);
        }
    }
    return out;
}

AssistantBrowserHandoff detect_assistant_browser_need(
    const Session& session, const ScrapeEndpointsResult& result,
    const ScrapeEndpointsOptions& options) {
    AssistantBrowserHandoff handoff;
    handoff.command = assistant_command(options);
    handoff.method = options.assistant_browser_method.empty()
        ? "webdriver"
        : options.assistant_browser_method;
    handoff.endpoint = options.assistant_browser_endpoint;
    handoff.debug_port = options.assistant_browser_debug_port;
    handoff.url = assistant_url(session);

    if (!options.assistant_browser_enabled) {
        return handoff;
    }
    if (const auto& detection = session.anti_bot_detection();
        detection.has_value() && detection->activated) {
        handoff.needed = true;
        handoff.reason = detection->category.empty()
            ? "anti-bot detection"
            : "anti-bot detection: " + detection->category;
        return handoff;
    }
    if (result.endpoints.empty() && session.document() != nullptr) {
        handoff.needed = true;
        handoff.reason = "no API endpoints discovered from current document";
    }
    return handoff;
}

void probe_spa(Session& session, const ScrapeEndpointsOptions& options) {
    if (!options.spa_probe || options.max_spa_actions == 0) return;
    auto document = session.document();
    if (document == nullptr) return;

    std::vector<std::shared_ptr<Element>> targets;
    const char* selectors[] = {"button", "input[type='submit']",
                               "input[type='button']", "[role='button']",
                               "a[href^='#']", "a[href^='javascript:']"};
    for (const char* selector : selectors) {
        for (auto& element : document->query_selector_all(selector)) {
            if (element != nullptr) targets.push_back(element);
            if (targets.size() >= options.max_spa_actions) break;
        }
        if (targets.size() >= options.max_spa_actions) break;
    }

    std::uint32_t actions = 0;
    for (auto& target : targets) {
        if (actions >= options.max_spa_actions) break;
        if (target != nullptr && target->click(session)) ++actions;
    }
}

ScrapeEndpointsResult scrape_from_document(const Document& document,
                                       const ScrapeEndpointsOptions& options,
                                       const Redactor& redactor) {
    EndpointExtractor extractor(to_extraction_opts(options));
    EndpointExtractionResult raw = extractor.extract(document);

    auto filtered = filter_to_api(raw.endpoints, options);

    ScrapeEndpointsResult result;
    result.endpoints = filtered;
    result.warnings = raw.warnings;

    // Wrap filtered endpoints in ResolvedEndpoint (unresolved)
    result.resolved.reserve(filtered.size());
    for (auto& e : filtered) {
        ResolvedEndpoint r;
        r.endpoint = e;
        r.final_url = e.url;
        r.status = 0;
        result.resolved.push_back(std::move(r));
    }
    result.openapi_yaml = render_scrape_yaml(result.resolved, options, redactor);
    result.postman_json = render_postman_json(result.resolved, options, redactor);
    return result;
}

ScrapeEndpointsResult scrape_from_session(Session& session,
                                      const ScrapeEndpointsOptions& options) {
    const Redactor redactor;
    auto document = session.document();
    if (document == nullptr) {
        ScrapeEndpointsResult empty;
        empty.warnings.push_back("session has no document");
        empty.openapi_yaml = render_scrape_yaml({}, options, redactor);
        empty.postman_json = render_postman_json({}, options, redactor);
        return empty;
    }
    probe_spa(session, options);
    document = session.document();
    if (document == nullptr) {
        ScrapeEndpointsResult empty;
        empty.warnings.push_back("session has no document after SPA probe");
        empty.openapi_yaml = render_scrape_yaml({}, options, redactor);
        empty.postman_json = render_postman_json({}, options, redactor);
        return empty;
    }
    EndpointExtractionOptions eo = to_extraction_opts(options);
    EndpointExtractor extractor(eo);
    if (options.observe_network) {
        for (const auto& call : session.page_script_requests()) {
            extractor.observe(call.method, call.url, call.status,
                              call.content_type);
        }
    }
    if (options.inspect_scripts) {
        for (const auto& text : session.page_script_texts()) {
            extractor.observe_script(text.url, text.body);
        }
    }

    EndpointExtractionResult raw = extractor.extract(*document);
    auto filtered = filter_to_api(raw.endpoints, options);

    ScrapeEndpointsResult result;
    result.endpoints = filtered;
    result.warnings = raw.warnings;
    result.resolved.reserve(filtered.size());

    if (!options.resolve_chain) {
        for (auto& e : filtered) {
            ResolvedEndpoint r;
            r.endpoint = e;
            r.final_url = e.url;
            result.resolved.push_back(std::move(r));
        }
        auto handoff = detect_assistant_browser_need(session, result, options);
        if (handoff.needed) {
            result.assistant_browser = handoff;
            result.warnings.push_back("assistant browser handoff recommended: " +
                                      handoff.reason);
        }
        result.openapi_yaml = render_scrape_yaml(result.resolved, options, redactor);
        result.postman_json = render_postman_json(result.resolved, options, redactor);
        return result;
    }

    // Resolve chain: BFS up to max_depth and max_resolve_requests
    struct QueueItem {
        DiscoveredEndpoint ep;
        int depth;
    };

    std::vector<QueueItem> queue;
    for (auto& e : filtered) queue.push_back({e, 0});

    std::map<std::string, size_t> visited; // url -> resolved index
    std::vector<ResolvedEndpoint> resolved;
    uint32_t fetches = 0;

    std::string base = document->base_url().empty() ? document->url() : document->base_url();

    while (!queue.empty() && fetches < options.max_resolve_requests) {
        QueueItem item = queue.front();
        queue.erase(queue.begin());

        std::string fetch_url = item.ep.url;
        // Resolve relative against base
        if (!base.empty()) {
            try {
                if (fetch_url.rfind("http://", 0) != 0 &&
                    fetch_url.rfind("https://", 0) != 0) {
                    fetch_url = resolve_url(base, fetch_url);
                }
            } catch (...) {}
        }

        if (visited.find(fetch_url) != visited.end()) continue;
        visited[fetch_url] = resolved.size();

        ResolvedEndpoint r;
        r.endpoint = item.ep;
        r.endpoint.url = fetch_url;
        r.final_url = fetch_url;

        if (fetches >= options.max_resolve_requests) {
            r.error = "resolve limit reached";
            resolved.push_back(std::move(r));
            continue;
        }

        try {
            HttpRequest req;
            req.method = "GET";
            req.url = fetch_url;
            req.headers = {};
            HttpResponse resp = session.request(req);
            r.status = resp.status;
            r.final_url = resp.final_url.empty() ? fetch_url : resp.final_url;
            r.redirect_chain = resp.redirect_chain;
            r.body = resp.body;
            auto ct = resp.header("Content-Type");
            r.content_type = ct;
            r.endpoint.response_content_type = ct;
            fetches++;

            // JSON link following
            if (options.follow_json_links && item.depth < static_cast<int>(options.max_depth)) {
                std::string body_lower = to_lower(ct);
                bool is_json = body_lower.find("json") != std::string::npos ||
                               (!resp.body.empty() && resp.body.front() == '{');
                if (is_json) {
                    auto discovered = extract_api_urls_from_body(resp.body, options.api_patterns);
                    r.json_discovered = discovered;
                    for (auto& du : discovered) {
                        std::string u = du;
                        try {
                            if (u.rfind("http", 0) != 0) {
                                u = resolve_url(r.final_url, u);
                            }
                            Url parsed = parse_url(u);
                            std::string path = parsed.path;
                            if (!is_api_path(path, options.api_patterns)) continue;
                            if (visited.find(u) != visited.end()) continue;
                            // Check not already queued
                            bool queued = false;
                            for (auto& q : queue) if (q.ep.url == u) { queued = true; break; }
                            if (queued) continue;
                            DiscoveredEndpoint ne;
                            ne.url = u;
                            ne.path = path;
                            ne.method = "get";
                            ne.source = r.final_url;
                            ne.discovery_method = "resolved-json";
                            ne.confidence = 0.60;
                            ne.notes.push_back("discovered in JSON response body");
                            queue.push_back({ne, item.depth + 1});
                        } catch (...) {}
                    }
                }
            }
        } catch (const std::exception& ex) {
            r.error = ex.what();
        } catch (...) {
            r.error = "unknown error during chain resolution";
        }

        resolved.push_back(std::move(r));
        // Merge newly discovered queue items already appended above.
    }

    // If we had filtered endpoints that were visited, resolved already contains them.
    // For any filtered endpoint that wasn't resolved due to limit, ensure it appears.
    for (auto& e : filtered) {
        std::string u = e.url;
        try {
            if (!base.empty() && u.rfind("http",0)!=0) u = resolve_url(base, u);
        } catch (...) {}
        if (visited.find(u) == visited.end()) {
            ResolvedEndpoint r;
            r.endpoint = e;
            r.final_url = u;
            resolved.push_back(std::move(r));
        }
    }

    result.resolved = std::move(resolved);
    // Derive endpoints list from resolved (unique by method+path)
    std::map<std::string, DiscoveredEndpoint> dedup;
    for (auto& r : result.resolved) {
        std::string key = r.endpoint.method + " " + r.endpoint.path;
        if (dedup.find(key) == dedup.end()) dedup[key] = r.endpoint;
    }
    result.endpoints.clear();
    for (auto& [k, ep] : dedup) result.endpoints.push_back(ep);
    std::sort(result.endpoints.begin(), result.endpoints.end(),
              [](const auto& a, const auto& b){
                  if (a.path != b.path) return a.path < b.path;
                  return a.method < b.method;
              });

    auto handoff = detect_assistant_browser_need(session, result, options);
    if (handoff.needed) {
        result.assistant_browser = handoff;
        result.warnings.push_back("assistant browser handoff recommended: " +
                                  handoff.reason);
    }
    result.openapi_yaml = render_scrape_yaml(result.resolved, options, redactor);
    result.postman_json = render_postman_json(result.resolved, options, redactor);
    return result;
}

ScrapeEndpointsResult scrape(Session& session, const ScrapeEndpointsOptions& options) {
    if (!options.html.empty()) {
        std::string base = options.base_url.empty() ? options.url : options.base_url;
        if (base.empty()) base = "https://example.com/";
        session.load_html(options.html, base);
        return scrape_from_session(session, options);
    }
    if (!options.url.empty()) {
        // Navigate only if not already at url; always attempt to ensure fresh doc
        try {
            session.navigate(options.url);
        } catch (...) {
            // If navigate fails, fall back to existing document if present
            if (session.document() == nullptr) throw;
        }
    }
    return scrape_from_session(session, options);
}

std::string render_scrape_yaml(const std::vector<ResolvedEndpoint>& resolved,
                               const ScrapeEndpointsOptions& options,
                               const Redactor& redactor) {
    std::ostringstream out;
    out << "openapi: " << options.openapi_version << "\n";
    out << "info:\n";
    out << "  title: 'Discovered API (scrape-endpoints)'\n";
    out << "  version: '0.1.0'\n";
    out << "  description: >-\n";
    out << "    Generated by ProwseTk scrape-endpoints plugin. Endpoints are\n";
    out << "    heuristic discoveries of suspected internal APIs; not\n";
    out << "    authoritative API documentation. Inferred values are marked and\n";
    out << "    carry provenance and confidence. When resolve_chain is enabled,\n";
    out << "    redirect chains and reachability are probed via host-mediated\n";
    out << "    requests.\n";
    out << "x-prowsetk-generated: true\n";
    out << "x-prowsetk-plugin: scrape-endpoints\n";
    if (options.assistant_browser_enabled) {
        out << "x-prowsetk-assistant-browser:\n";
        out << "  enabled: true\n";
        out << "  method: " << yaml_quote(options.assistant_browser_method.empty()
            ? "webdriver" : options.assistant_browser_method) << "\n";
        const auto command = assistant_command(options);
        out << "  command: " << yaml_quote(command) << "\n";
        if (!options.assistant_browser_endpoint.empty()) {
            out << "  endpoint: " << yaml_quote(options.assistant_browser_endpoint) << "\n";
        }
        if (options.assistant_browser_debug_port != 0) {
            out << "  debug-port: " << options.assistant_browser_debug_port << "\n";
        }
        out << "  note: 'User-assisted browser handoff is optional and heuristic.'\n";
    }

    // Group by path
    std::map<std::string, std::vector<const ResolvedEndpoint*>> by_path;
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
        for (const auto* re : entries) {
            const auto& ep = re->endpoint;
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
            std::string ct = re->content_type.empty() ? ep.response_content_type : re->content_type;
            if (!ct.empty()) {
                out << "      responses:\n";
                out << "        '" << (re->status ? std::to_string(re->status) : "200") << "':\n";
                out << "          description: " << (re->status ? "Observed response" : "Inferred response") << "\n";
                out << "          content:\n";
                out << "            " << yaml_quote(ct) << ":\n";
                out << "              schema:\n";
                out << "                type: object\n";
                out << "                x-inferred: true\n";
            } else {
                out << "      responses:\n";
                out << "        '" << (re->status ? std::to_string(re->status) : "200") << "':\n";
                out << "          description: "
                    << (re->status ? "Observed response" : "Inferred response")
                    << "\n";
            }
            if (options.infer_schemas) {
                out << "      x-inferred: true\n";
            }
            if (options.include_provenance) {
                std::string source = options.redact_secrets ? redactor.redact_url(ep.source) : ep.source;
                std::string url = options.redact_secrets ? redactor.redact_url(ep.url) : ep.url;
                std::string final_url = options.redact_secrets ? redactor.redact_url(re->final_url) : re->final_url;
                out << "      x-prowsetk-provenance:\n";
                out << "        source: " << yaml_quote(source) << "\n";
                out << "        url: " << yaml_quote(url) << "\n";
                if (!final_url.empty() && final_url != url) {
                    out << "        final-url: " << yaml_quote(final_url) << "\n";
                }
                out << "        discovery-method: " << yaml_quote(ep.discovery_method) << "\n";
                out << "        confidence: " << format_conf(ep.confidence) << "\n";
                out << "        inferred: true\n";
                if (!re->redirect_chain.empty()) {
                    out << "        redirect-chain:\n";
                    for (const auto& u : re->redirect_chain) {
                        std::string ru = options.redact_secrets ? redactor.redact_url(u) : u;
                        out << "          - " << yaml_quote(ru) << "\n";
                    }
                }
                if (re->status != 0) {
                    out << "        http-status: " << re->status << "\n";
                }
                if (!re->error.empty()) {
                    out << "        resolve-error: " << yaml_quote(re->error) << "\n";
                }
                if (!ep.notes.empty()) {
                    out << "        notes:\n";
                    for (const auto& n : ep.notes) out << "          - " << yaml_quote(n) << "\n";
                }
                if (!re->json_discovered.empty()) {
                    out << "        json-discovered:\n";
                    for (const auto& j : re->json_discovered) {
                        std::string rj = options.redact_secrets ? redactor.redact_url(j) : j;
                        out << "          - " << yaml_quote(rj) << "\n";
                    }
                }
            }
            if (options.resolve_chain) {
                // Always emit chain even when include_provenance false, but redacted
                if (!options.include_provenance && !re->redirect_chain.empty()) {
                    out << "      x-prowsetk-chain:\n";
                    out << "        final-url: " << yaml_quote(options.redact_secrets ? redactor.redact_url(re->final_url) : re->final_url) << "\n";
                    out << "        redirect-chain:\n";
                    for (const auto& u : re->redirect_chain) {
                        out << "          - " << yaml_quote(options.redact_secrets ? redactor.redact_url(u) : u) << "\n";
                    }
                }
            }
        }
    }
    return out.str();
}

namespace {

std::string json_quote(std::string_view value) {
    constexpr char hex[] = "0123456789abcdef";
    std::string out = "\"";
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += static_cast<char>(c);
        } else if (c < 0x20) {
            out += "\\u00";
            out += hex[c >> 4];
            out += hex[c & 15];
        } else {
            out += static_cast<char>(c);
        }
    }
    return out + '"';
}

std::string decode_query_key(std::string_view key) {
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string out;
    for (std::size_t i = 0; i < key.size(); ++i) {
        if (key[i] == '%' && i + 2 < key.size() && hex(key[i + 1]) >= 0 &&
            hex(key[i + 2]) >= 0) {
            out += static_cast<char>(hex(key[i + 1]) * 16 + hex(key[i + 2]));
            i += 2;
        } else {
            out += key[i] == '+' ? ' ' : key[i];
        }
    }
    return out;
}

std::string safe_postman_url(const std::string& value, bool redact,
                             const Redactor& redactor) {
    if (!redact || value.empty()) return value;
    try {
        auto url = parse_url(value);
        url.userinfo.clear();
        url.fragment.clear();
        url.has_fragment = false;
        std::string query;
        for (std::size_t start = 0; start < url.query.size();) {
            const auto end = url.query.find('&', start);
            const auto pair = url.query.substr(
                start, end == std::string::npos ? end : end - start);
            const auto eq = pair.find('=');
            const auto key = pair.substr(0, eq);
            if (!query.empty()) query += '&';
            query += redactor.is_sensitive_query_parameter(decode_query_key(key))
                ? key + "=" + redactor.policy().replacement
                : pair;
            if (end == std::string::npos) break;
            start = end + 1;
        }
        url.query = query;
        return url.to_string();
    } catch (...) {
        return redactor.policy().replacement;
    }
}

std::string postman_method_name(std::string method) {
    for (char& c : method) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return method.empty() ? "GET" : method;
}

std::string postman_request_url(const DiscoveredEndpoint& ep) {
    if (ep.request_content_type == "application/x-www-form-urlencoded") return ep.url;
    auto url = parse_url(ep.url);
    std::vector<std::string> names;
    for (std::size_t start = 0; start < url.query.size();) {
        const auto end = url.query.find('&', start);
        const auto pair = url.query.substr(
            start, end == std::string::npos ? end : end - start);
        names.push_back(decode_query_key(pair.substr(0, pair.find('='))));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    constexpr char hex[] = "0123456789ABCDEF";
    for (const auto& param : ep.parameters) {
        if (std::find(names.begin(), names.end(), param) != names.end()) continue;
        names.push_back(param);
        if (!url.query.empty()) url.query += '&';
        for (unsigned char c : param) {
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
                c == '~') {
                url.query += static_cast<char>(c);
            } else {
                url.query += '%';
                url.query += hex[c >> 4];
                url.query += hex[c & 15];
            }
        }
        url.query += '=';
        url.has_query = true;
    }
    return url.to_string();
}

}  // namespace

std::string render_postman_json(const std::vector<ResolvedEndpoint>& resolved,
                                const ScrapeEndpointsOptions& options,
                                const Redactor& redactor) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    std::string description =
        "Generated by ProwseTk scrape-endpoints. Heuristic API discoveries; "
        "not authoritative documentation. Request bodies and credentials are omitted.";
    if (options.spa_probe) {
        description += " SPA probing used Flatworm synthetic interactions before export.";
    }
    if (options.assistant_browser_enabled) {
        description += " Assistant browser handoff is enabled via ";
        description += options.assistant_browser_method.empty()
            ? "webdriver"
            : options.assistant_browser_method;
        description += "; any user-assisted coverage remains heuristic.";
    }

    out << "{\n  \"info\": {\"name\": " << json_quote(options.collection_name)
        << ", \"schema\": \"https://schema.getpostman.com/json/collection/v2.1.0/collection.json\","
        << " \"description\": " << json_quote(description) << "},\n  \"item\": [";

    std::map<std::pair<std::string, std::string>, const ResolvedEndpoint*> ordered;
    for (const auto& r : resolved) {
        ordered.try_emplace(
            std::make_pair(r.endpoint.url, postman_method_name(r.endpoint.method)), &r);
    }

    bool first = true;
    for (const auto& [key, record] : ordered) {
        const auto& ep = record->endpoint;
        const auto url = safe_postman_url(postman_request_url(ep),
                                          options.redact_secrets, redactor);
        const auto& method = key.second;
        if (!first) out << ',';
        first = false;
        std::ostringstream item_description;
        item_description.imbue(std::locale::classic());
        item_description << "Heuristic discovery; inferred request. Confidence: "
                         << (std::isfinite(ep.confidence)
                                 ? std::clamp(ep.confidence, 0.0, 1.0)
                                 : 0.0);
        if (options.include_provenance) {
            item_description << "\nSource: "
                             << safe_postman_url(ep.source, options.redact_secrets,
                                                 redactor)
                             << "\nDiscovery method: " << ep.discovery_method;
            if (!record->final_url.empty()) {
                item_description << "\nFinal URL: "
                                 << safe_postman_url(record->final_url,
                                                     options.redact_secrets,
                                                     redactor);
            }
            for (const auto& hop : record->redirect_chain) {
                item_description << "\nRedirect: "
                                 << safe_postman_url(hop, options.redact_secrets,
                                                     redactor);
            }
            for (const auto& link : record->json_discovered) {
                item_description << "\nJSON link: "
                                 << safe_postman_url(link, options.redact_secrets,
                                                     redactor);
            }
            if (record->status != 0) {
                item_description << "\nObserved HTTP status: " << record->status;
            }
            if (!record->error.empty()) item_description << "\nResolution failed.";
        }

        out << "\n    {\"name\": " << json_quote(method + " " + url)
            << ", \"request\": {\"method\": " << json_quote(method)
            << ", \"url\": " << json_quote(url)
            << ", \"description\": " << json_quote(item_description.str())
            << ", \"header\": [";
        if (!ep.request_content_type.empty()) {
            out << "{\"key\": \"Content-Type\", \"value\": "
                << json_quote(ep.request_content_type) << '}';
        }
        out << ']';
        if (ep.request_content_type == "application/x-www-form-urlencoded") {
            out << ", \"body\": {\"mode\": \"urlencoded\", \"urlencoded\": [";
            bool first_field = true;
            for (const auto& param : ep.parameters) {
                if (!first_field) out << ',';
                first_field = false;
                out << "{\"key\": " << json_quote(param)
                    << ", \"value\": \"\", \"description\": \"Inferred field; supply a value\"}";
            }
            out << "]}";
        }
        out << "}, \"response\": []}";
    }
    out << "\n  ]\n}\n";
    return out.str();
}

void ScrapeEndpointsResult::write_openapi_yaml(
    const std::filesystem::path& path) const {
    std::ofstream file;
    file.exceptions(std::ios::failbit | std::ios::badbit);
    file.open(path, std::ios::binary | std::ios::trunc);
    file << openapi_yaml;
    file.close();
}

void ScrapeEndpointsResult::write_postman_json(
    const std::filesystem::path& path) const {
    std::ofstream file;
    file.exceptions(std::ios::failbit | std::ios::badbit);
    file.open(path, std::ios::binary | std::ios::trunc);
    file << postman_json;
    file.close();
}

}  // namespace prowsetk::plugins::scrape_endpoints
