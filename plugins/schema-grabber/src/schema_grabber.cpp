#include "prowsetk/plugins/schema_grabber.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <locale>
#include <map>
#include <set>
#include <sstream>

#include "prowsetk/url.hpp"

namespace prowsetk::plugins::schema_grabber {
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
                                          "/ajax", "/gateway", "/service",
                                          "/backend", "/bff", "/dml",
                                          "/internal", "/private",
                                          "/hotel/hoteladmin",
                                          "/partner-settings",
                                          "/telemetry", "challenge",
                                          "/beacon", "/collect"};
    const std::string lower = to_lower(path);
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

std::string truncate_example(std::string v, std::size_t max_chars,
                             const std::string& replacement, bool sensitive) {
    if (sensitive) return replacement;
    if (v.size() > max_chars) v.resize(max_chars);
    // Strip control characters that would break YAML/JSON emitters.
    for (char& c : v) {
        if (static_cast<unsigned char>(c) < 0x20 && c != '\t') c = ' ';
    }
    return v;
}

std::string decode_component(std::string_view in) {
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string out;
    for (std::size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%' && i + 2 < in.size() && hex(in[i + 1]) >= 0 &&
            hex(in[i + 2]) >= 0) {
            out += static_cast<char>(hex(in[i + 1]) * 16 + hex(in[i + 2]));
            i += 2;
        } else if (in[i] == '+') {
            out += ' ';
        } else {
            out += in[i];
        }
    }
    return out;
}

bool is_integer_text(const std::string& v) {
    if (v.empty()) return false;
    std::size_t i = (v[0] == '-' || v[0] == '+') ? 1 : 0;
    if (i >= v.size()) return false;
    for (; i < v.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(v[i]))) return false;
    }
    return true;
}

bool is_number_text(const std::string& v) {
    if (v.empty()) return false;
    try {
        std::size_t pos = 0;
        std::stod(v, &pos);
        return pos == v.size();
    } catch (...) {
        return false;
    }
}

bool is_uuid_text(const std::string& v) {
    // 8-4-4-4-12 hex
    if (v.size() != 36) return false;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (v[i] != '-') return false;
        } else if (!std::isxdigit(static_cast<unsigned char>(v[i]))) {
            return false;
        }
    }
    return true;
}

bool is_long_hex(const std::string& v) {
    if (v.size() < 20 || v.size() > 64) return false;
    for (char c : v) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

bool looks_like_id_segment(const std::string& seg) {
    if (seg.empty()) return false;
    if (is_integer_text(seg)) return true;
    if (is_uuid_text(seg)) return true;
    if (is_long_hex(seg)) return true;
    // Mongo-style 24-hex is covered above; catch 20+ char alnum tokens.
    if (seg.size() >= 20) {
        bool alnum = true;
        for (char c : seg) {
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' &&
                c != '_') {
                alnum = false;
                break;
            }
        }
        if (alnum) return true;
    }
    return false;
}

// --- Minimal JSON parser (objects, arrays, scalars) ------------------------

struct JsonValue {
    enum class Kind { Null, Bool, Integer, Number, String, Array, Object };
    Kind kind = Kind::Null;
    bool boolean = false;
    std::string string;
    std::vector<JsonValue> array;
    std::vector<std::pair<std::string, JsonValue>> object;
};

struct JsonParser {
    const char* cur;
    const char* end;
    bool failed = false;

    explicit JsonParser(std::string_view text)
        : cur(text.data()), end(text.data() + text.size()) {}

    void skip_ws() {
        while (cur < end &&
               (*cur == ' ' || *cur == '\t' || *cur == '\n' || *cur == '\r'))
            ++cur;
    }
    bool expect(char c) {
        skip_ws();
        if (cur < end && *cur == c) {
            ++cur;
            return true;
        }
        failed = true;
        return false;
    }
    bool literal(const char* word) {
        skip_ws();
        for (const char* p = word; *p; ++p) {
            if (cur >= end || *cur != *p) {
                failed = true;
                return false;
            }
            ++cur;
        }
        return true;
    }
    JsonValue parse_string() {
        JsonValue v;
        v.kind = JsonValue::Kind::String;
        if (!expect('"')) return v;
        while (cur < end && *cur != '"') {
            if (*cur == '\\' && cur + 1 < end) {
                ++cur;
                switch (*cur) {
                    case '"': v.string += '"'; break;
                    case '\\': v.string += '\\'; break;
                    case '/': v.string += '/'; break;
                    case 'b': v.string += '\b'; break;
                    case 'f': v.string += '\f'; break;
                    case 'n': v.string += '\n'; break;
                    case 'r': v.string += '\r'; break;
                    case 't': v.string += '\t'; break;
                    default: v.string += *cur; break;
                }
                ++cur;
            } else {
                v.string += *cur++;
            }
        }
        if (!expect('"')) failed = true;
        return v;
    }
    JsonValue parse_value() {
        skip_ws();
        JsonValue v;
        if (cur >= end) {
            failed = true;
            return v;
        }
        if (*cur == '{') {
            ++cur;
            v.kind = JsonValue::Kind::Object;
            skip_ws();
            if (cur < end && *cur == '}') {
                ++cur;
                return v;
            }
            while (true) {
                JsonValue key = parse_string();
                if (failed) return v;
                if (!expect(':')) return v;
                JsonValue val = parse_value();
                if (failed) return v;
                v.object.emplace_back(key.string, std::move(val));
                skip_ws();
                if (cur < end && *cur == ',') {
                    ++cur;
                    continue;
                }
                break;
            }
            if (!expect('}')) failed = true;
            return v;
        }
        if (*cur == '[') {
            ++cur;
            v.kind = JsonValue::Kind::Array;
            skip_ws();
            if (cur < end && *cur == ']') {
                ++cur;
                return v;
            }
            while (true) {
                v.array.push_back(parse_value());
                if (failed) return v;
                skip_ws();
                if (cur < end && *cur == ',') {
                    ++cur;
                    continue;
                }
                break;
            }
            if (!expect(']')) failed = true;
            return v;
        }
        if (*cur == '"') return parse_string();
        if (*cur == 't') {
            if (literal("true")) {
                v.kind = JsonValue::Kind::Bool;
                v.boolean = true;
            }
            return v;
        }
        if (*cur == 'f') {
            if (literal("false")) {
                v.kind = JsonValue::Kind::Bool;
                v.boolean = false;
            }
            return v;
        }
        if (*cur == 'n') {
            literal("null");
            v.kind = JsonValue::Kind::Null;
            return v;
        }
        // number
        const char* start = cur;
        if (*cur == '-') ++cur;
        while (cur < end &&
               (std::isdigit(static_cast<unsigned char>(*cur)) || *cur == '.' ||
                *cur == 'e' || *cur == 'E' || *cur == '+' || *cur == '-'))
            ++cur;
        if (cur == start) {
            failed = true;
            return v;
        }
        v.string.assign(start, cur);
        const bool is_int = is_integer_text(v.string);
        v.kind = is_int ? JsonValue::Kind::Integer : JsonValue::Kind::Number;
        return v;
    }
};

std::string guess_string_format(const std::string& name,
                                const std::string& value) {
    const std::string lname = to_lower(name);
    const std::string lval = to_lower(value);
    if (lname.find("email") != std::string::npos ||
        (value.find('@') != std::string::npos &&
         value.find('.') != std::string::npos))
        return "email";
    if (lname.find("url") != std::string::npos ||
        lname.find("uri") != std::string::npos || lname.find("link") != std::string::npos ||
        lval.rfind("http://", 0) == 0 || lval.rfind("https://", 0) == 0)
        return "uri";
    if (lname.find("uuid") != std::string::npos || is_uuid_text(value))
        return "uuid";
    if (lname.find("date") != std::string::npos ||
        lname.find("time") != std::string::npos ||
        lname == "created_at" || lname == "updated_at") {
        // Heuristic ISO-8601 sniff: 2024-01-31... or ...T...Z
        if (value.size() >= 10 && value[4] == '-' && value[7] == '-') return "date-time";
    }
    return {};
}

JsonField field_from_json(const std::string& name, const JsonValue& v,
                          const SchemaGrabberOptions& options,
                          const Redactor& redactor, std::uint32_t depth,
                          bool* truncated_inout, const std::string& provenance);

InferredSchema schema_from_object(const std::vector<std::pair<std::string, JsonValue>>& members,
                                  const SchemaGrabberOptions& options,
                                  const Redactor& redactor, std::uint32_t depth,
                                  bool* truncated_inout,
                                  const std::string& provenance, double confidence) {
    InferredSchema schema;
    schema.provenance = provenance;
    schema.confidence = confidence;
    schema.truncated = false;
    std::size_t count = 0;
    for (const auto& [key, val] : members) {
        if (count >= options.max_properties) {
            schema.truncated = true;
            if (truncated_inout != nullptr) *truncated_inout = true;
            break;
        }
        schema.properties.push_back(
            field_from_json(key, val, options, redactor, depth, truncated_inout, provenance));
        ++count;
    }
    std::sort(schema.properties.begin(), schema.properties.end(),
              [](const JsonField& a, const JsonField& b) { return a.name < b.name; });
    return schema;
}

JsonField field_from_json(const std::string& name, const JsonValue& v,
                          const SchemaGrabberOptions& options,
                          const Redactor& redactor, std::uint32_t depth,
                          bool* truncated_inout, const std::string& provenance) {
    JsonField f;
    f.name = name;
    f.provenance = provenance;
    f.sensitive = redactor.is_sensitive_query_parameter(name);
    const std::string replacement = redactor.policy().replacement;
    switch (v.kind) {
        case JsonValue::Kind::Null:
            f.type = "string";
            break;
        case JsonValue::Kind::Bool:
            f.type = "boolean";
            if (options.include_examples)
                f.example = v.boolean ? "true" : "false";
            break;
        case JsonValue::Kind::Integer:
            f.type = "integer";
            if (options.include_examples)
                f.example = truncate_example(v.string, options.max_example_chars,
                                             replacement, false);
            break;
        case JsonValue::Kind::Number:
            f.type = "number";
            if (options.include_examples)
                f.example = truncate_example(v.string, options.max_example_chars,
                                             replacement, false);
            break;
        case JsonValue::Kind::String:
            f.type = "string";
            f.format = guess_string_format(name, v.string);
            if (options.include_examples)
                f.example = truncate_example(v.string, options.max_example_chars,
                                             replacement, f.sensitive);
            break;
        case JsonValue::Kind::Array: {
            f.type = "array";
            if (!v.array.empty() && depth < options.max_depth) {
                // Merge element kinds deterministically.
                std::set<std::string> kinds;
                for (const auto& el : v.array) {
                    switch (el.kind) {
                        case JsonValue::Kind::Integer: kinds.insert("integer"); break;
                        case JsonValue::Kind::Number: kinds.insert("number"); break;
                        case JsonValue::Kind::Bool: kinds.insert("boolean"); break;
                        case JsonValue::Kind::String: kinds.insert("string"); break;
                        case JsonValue::Kind::Object: kinds.insert("object"); break;
                        case JsonValue::Kind::Array: kinds.insert("array"); break;
                        default: kinds.insert("string"); break;
                    }
                }
                if (kinds.size() == 1) {
                    f.items_type = *kinds.begin();
                } else {
                    f.items_type = "string";
                    f.provenance += " (mixed array items)";
                }
                if (f.items_type == "object") {
                    // Merge object element properties by name (first wins).
                    std::map<std::string, JsonValue> merged;
                    for (const auto& el : v.array) {
                        if (el.kind != JsonValue::Kind::Object) continue;
                        for (const auto& [k, val] : el.object) {
                            if (merged.find(k) == merged.end()) merged[k] = val;
                            if (merged.size() >= options.max_properties) break;
                        }
                    }
                    for (const auto& [k, val] : merged) {
                        f.items_properties.push_back(field_from_json(
                            k, val, options, redactor, depth + 1, truncated_inout,
                            provenance));
                    }
                    std::sort(f.items_properties.begin(), f.items_properties.end(),
                              [](const JsonField& a, const JsonField& b) {
                                  return a.name < b.name;
                              });
                }
            } else {
                f.items_type = "string";
                if (!v.array.empty() && truncated_inout != nullptr)
                    *truncated_inout = true;
            }
            break;
        }
        case JsonValue::Kind::Object: {
            f.type = "object";
            if (depth < options.max_depth) {
                std::size_t count = 0;
                for (const auto& [k, val] : v.object) {
                    if (count >= options.max_properties) {
                        if (truncated_inout != nullptr) *truncated_inout = true;
                        break;
                    }
                    f.properties.push_back(field_from_json(
                        k, val, options, redactor, depth + 1, truncated_inout,
                        provenance));
                    ++count;
                }
                std::sort(f.properties.begin(), f.properties.end(),
                          [](const JsonField& a, const JsonField& b) {
                              return a.name < b.name;
                          });
            } else if (truncated_inout != nullptr) {
                *truncated_inout = true;
            }
            break;
        }
    }
    return f;
}

EndpointExtractionOptions to_extraction_opts(const SchemaGrabberOptions& o) {
    EndpointExtractionOptions e;
    e.follow_links = true;
    e.inspect_scripts = o.inspect_scripts;
    e.observe_network = true;
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

// Scans inline scripts for a JSON body hint tied to `path`: looks for
// JSON.stringify({...}) or a "body": {...} literal within a bounded window of
// the path mention. Returns the balanced {...} text or empty.
std::string json_body_hint_for(const Document& document, const std::string& path) {
    if (path.empty()) return {};
    for (const auto& script : document.scripts()) {
        if (script->has_attribute("src")) continue;
        const std::string text = script->text();
        std::size_t pos = 0;
        while ((pos = text.find(path, pos)) != std::string::npos) {
            const std::size_t from = pos > 800 ? pos - 800 : 0;
            const std::size_t to = std::min(text.size(), pos + 800);
            const std::string window = text.substr(from, to - from);
            // JSON.stringify( { ... } )
            std::size_t anchor = window.find("JSON.stringify");
            if (anchor == std::string::npos) anchor = window.find("body");
            if (anchor == std::string::npos) {
                pos += path.size();
                continue;
            }
            const std::size_t open = window.find('{', anchor);
            if (open == std::string::npos) {
                pos += path.size();
                continue;
            }
            int depth = 0;
            bool in_str = false;
            char quote = 0;
            for (std::size_t i = open; i < window.size() && i < open + 4096; ++i) {
                const char c = window[i];
                if (in_str) {
                    if (c == '\\') {
                        ++i;
                        continue;
                    }
                    if (c == quote) in_str = false;
                    continue;
                }
                if (c == '"' || c == '\'') {
                    in_str = true;
                    quote = c;
                } else if (c == '{') {
                    ++depth;
                } else if (c == '}') {
                    if (--depth == 0) return window.substr(open, i - open + 1);
                }
            }
            pos += path.size();
        }
    }
    return {};
}

std::string input_type_to_openapi(const std::string& input_type,
                                  std::string* format_out) {
    const std::string t = to_lower(input_type);
    if (t == "number" || t == "range") return "number";
    if (t == "checkbox") return "boolean";
    if (t == "email" && format_out != nullptr) {
        *format_out = "email";
        return "string";
    }
    if ((t == "url") && format_out != nullptr) {
        *format_out = "uri";
        return "string";
    }
    if (t == "date" && format_out != nullptr) {
        *format_out = "date";
        return "string";
    }
    if (t == "datetime-local" || t == "datetime") {
        if (format_out != nullptr) *format_out = "date-time";
        return "string";
    }
    return "string";
}

}  // namespace

bool is_schema_api_path(const std::string& path,
                        const std::vector<std::string>& patterns) {
    if (patterns.empty()) return builtin_api_marker(path);
    for (const auto& pat : patterns) {
        if (path_contains(path, pat)) return true;
    }
    return builtin_api_marker(path);
}

std::vector<DiscoveredEndpoint> filter_to_api(
    const std::vector<DiscoveredEndpoint>& endpoints,
    const SchemaGrabberOptions& options) {
    if (!options.require_api_pattern) return endpoints;
    std::vector<DiscoveredEndpoint> out;
    out.reserve(endpoints.size());
    for (const auto& e : endpoints) {
        if (is_schema_api_path(e.path, options.api_patterns)) {
            out.push_back(e);
            continue;
        }
        // An explicit non-GET method (POST form, fetch/XHR POST, beacon) is
        // itself evidence of a backend surface.
        if (to_lower(e.method) != "get") out.push_back(e);
    }
    return out;
}

std::string templatize_path(const std::string& path,
                            std::vector<PathParam>* params) {
    if (path.empty() || path == "/") return "/";
    std::string out;
    std::size_t i = 0;
    while (i < path.size()) {
        if (path[i] == '/') {
            out += '/';
            ++i;
            continue;
        }
        std::size_t j = path.find('/', i);
        if (j == std::string::npos) j = path.size();
        std::string seg = path.substr(i, j - i);
        // Leave existing templates alone.
        if (seg.size() >= 2 && seg.front() == '{' && seg.back() == '}') {
            out += seg;
            if (params != nullptr) {
                PathParam p;
                p.name = seg.substr(1, seg.size() - 2);
                params->push_back(std::move(p));
            }
        } else if (looks_like_id_segment(seg)) {
            out += "{id}";
            if (params != nullptr) {
                bool exists = false;
                for (const auto& p : *params) {
                    if (p.name == "id") {
                        exists = true;
                        break;
                    }
                }
                if (!exists) {
                    PathParam p;
                    p.name = "id";
                    p.example = seg;
                    params->push_back(std::move(p));
                }
            }
        } else {
            out += seg;
        }
        i = j;
    }
    return out.empty() ? "/" : out;
}

std::string infer_scalar_type(const std::string& value) {
    const std::string lower = to_lower(value);
    if (lower == "true" || lower == "false") return "boolean";
    if (is_integer_text(value)) return "integer";
    if (is_number_text(value)) return "number";
    return "string";
}

std::vector<QueryParam> query_params_for(const std::string& url,
                                         const Redactor& redactor, bool redact) {
    std::vector<QueryParam> out;
    std::string query;
    try {
        query = parse_url(url).query;
    } catch (...) {
        return out;
    }
    std::size_t start = 0;
    while (start <= query.size()) {
        const auto amp = query.find('&', start);
        const std::string pair = amp == std::string::npos
                                     ? query.substr(start)
                                     : query.substr(start, amp - start);
        const auto eq = pair.find('=');
        const std::string raw_name =
            eq == std::string::npos ? pair : pair.substr(0, eq);
        const std::string raw_value =
            eq == std::string::npos ? "" : pair.substr(eq + 1);
        const std::string name = decode_component(raw_name);
        const std::string value = decode_component(raw_value);
        if (!name.empty()) {
            const bool sensitive = redactor.is_sensitive_query_parameter(name);
            bool known = false;
            for (auto& q : out) {
                if (q.name == name) {
                    known = true;
                    if (q.example.empty() && !value.empty() && !sensitive)
                        q.example = value;
                    break;
                }
            }
            if (!known) {
                QueryParam qp;
                qp.name = name;
                qp.type = value.empty() ? "string" : infer_scalar_type(value);
                qp.required = false;
                qp.sensitive = sensitive;
                qp.example = (sensitive && redact) ? redactor.policy().replacement
                                                  : value;
                if (!redact && sensitive) qp.example = value;
                out.push_back(std::move(qp));
            }
        }
        if (amp == std::string::npos) break;
        start = amp + 1;
    }
    std::sort(out.begin(), out.end(),
              [](const QueryParam& a, const QueryParam& b) { return a.name < b.name; });
    return out;
}

std::optional<InferredSchema> infer_json_schema(const std::string& body,
                                                const SchemaGrabberOptions& options,
                                                const Redactor& redactor,
                                                std::string provenance) {
    if (body.empty()) return std::nullopt;
    const std::size_t start = body.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return std::nullopt;
    const char first = body[start];
    if (first != '{' && first != '[') return std::nullopt;
    JsonParser parser{std::string_view(body)};
    JsonValue root = parser.parse_value();
    if (parser.failed) return std::nullopt;
    bool truncated = body.size() > options.max_body_bytes;
    if (root.kind == JsonValue::Kind::Object) {
        InferredSchema schema = schema_from_object(root.object, options, redactor,
                                                   0, &truncated, provenance, 0.80);
        schema.truncated = truncated;
        return schema;
    }
    if (root.kind == JsonValue::Kind::Array) {
        // Top-level arrays become an array schema with merged item fields.
        InferredSchema schema;
        schema.type = "array";
        schema.provenance = std::move(provenance);
        schema.confidence = 0.80;
        schema.truncated = truncated;
        std::map<std::string, JsonValue> merged;
        for (const auto& el : root.array) {
            if (el.kind != JsonValue::Kind::Object) continue;
            for (const auto& [k, val] : el.object) {
                if (merged.find(k) == merged.end()) merged[k] = val;
                if (merged.size() >= options.max_properties) break;
            }
            if (merged.size() >= options.max_properties) {
                truncated = true;
                schema.truncated = true;
                break;
            }
        }
        for (const auto& [k, val] : merged) {
            schema.properties.push_back(field_from_json(
                k, val, options, redactor, 1, &truncated, schema.provenance));
        }
        std::sort(schema.properties.begin(), schema.properties.end(),
                  [](const JsonField& a, const JsonField& b) {
                      return a.name < b.name;
                  });
        schema.truncated = truncated;
        return schema;
    }
    return std::nullopt;
}

std::vector<JsonField> form_fields_for(const Document& document,
                                       const DiscoveredEndpoint& endpoint,
                                       const SchemaGrabberOptions& options,
                                       const Redactor& redactor) {
    std::vector<JsonField> out;
    const std::string base =
        document.base_url().empty() ? document.url() : document.base_url();
    const std::string want_method = to_lower(endpoint.method);
    for (const auto& form : document.forms()) {
        const std::string action = form->attribute("action");
        std::string resolved;
        try {
            resolved = action.empty()
                           ? endpoint.url
                           : (base.empty() ? action : resolve_url(base, action));
        } catch (...) {
            continue;
        }
        std::string form_path;
        try {
            Url parsed = parse_url(resolved);
            form_path = parsed.path.empty() ? "/" : parsed.path;
        } catch (...) {
            continue;
        }
        std::string form_method = to_lower(form->attribute("method"));
        if (form_method.empty()) form_method = "get";
        if (form_method != "post" && form_method != "get") form_method = want_method;
        if (form_path != endpoint.path || form_method != want_method) continue;
        for (const auto& control :
             form->query_selector_all("input[name], select[name], textarea[name]")) {
            const std::string name = control->attribute("name");
            if (name.empty()) continue;
            bool known = false;
            for (const auto& f : out) {
                if (f.name == name) {
                    known = true;
                    break;
                }
            }
            if (known) continue;
            JsonField field;
            field.name = name;
            field.provenance = "form-field";
            field.sensitive = redactor.is_sensitive_query_parameter(name);
            const std::string tag = to_lower(control->tag_name());
            if (tag == "textarea" || tag == "select") {
                field.type = "string";
            } else {
                std::string format;
                field.type =
                    input_type_to_openapi(control->attribute("type"), &format);
                field.format = format;
            }
            if (field.type == "string" && field.format.empty())
                field.format = guess_string_format(name, control->attribute("value"));
            field.required = control->has_attribute("required");
            if (options.include_examples) {
                field.example = truncate_example(control->attribute("value"),
                                                 options.max_example_chars,
                                                 redactor.policy().replacement,
                                                 options.redact_secrets &&
                                                     field.sensitive);
            }
            out.push_back(std::move(field));
        }
    }
    std::sort(out.begin(), out.end(),
              [](const JsonField& a, const JsonField& b) { return a.name < b.name; });
    return out;
}

std::optional<InferredSchema> request_schema_for(
    const DiscoveredEndpoint& endpoint, const Document* document,
    const SchemaGrabberOptions& options, const Redactor& redactor,
    std::string* provenance_out) {
    const std::string method = to_lower(endpoint.method);
    const bool has_body_method =
        method == "post" || method == "put" || method == "patch" || method == "delete";
    // Form fields win: they are the strongest request-body signal.
    if (document != nullptr && has_body_method) {
        auto fields = form_fields_for(*document, endpoint, options, redactor);
        if (!fields.empty()) {
            InferredSchema schema;
            schema.provenance = "form-fields";
            schema.confidence = 0.75;
            schema.properties = std::move(fields);
            for (const auto& f : schema.properties) {
                if (f.required) schema.required.push_back(f.name);
            }
            std::sort(schema.required.begin(), schema.required.end());
            if (provenance_out != nullptr) *provenance_out = schema.provenance;
            return schema;
        }
    }
    // JSON body hints in inline scripts (fetch bodies for this path).
    if (document != nullptr && has_body_method) {
        const std::string hint = json_body_hint_for(*document, endpoint.path);
        if (!hint.empty()) {
            auto schema = infer_json_schema(hint, options, redactor, "script-body-literal");
            if (schema.has_value()) {
                schema->confidence = 0.60;
                if (provenance_out != nullptr) *provenance_out = schema->provenance;
                return schema;
            }
        }
    }
    // Fall back to a generic object when a body content type is known.
    if (has_body_method && !endpoint.request_content_type.empty()) {
        InferredSchema schema;
        schema.provenance = "content-type-hint";
        schema.confidence = 0.40;
        if (!endpoint.parameters.empty()) {
            for (const auto& name : endpoint.parameters) {
                JsonField f;
                f.name = name;
                f.type = "string";
                f.provenance = schema.provenance;
                f.sensitive = redactor.is_sensitive_query_parameter(name);
                schema.properties.push_back(std::move(f));
            }
            std::sort(schema.properties.begin(), schema.properties.end(),
                      [](const JsonField& a, const JsonField& b) {
                          return a.name < b.name;
                      });
        }
        if (provenance_out != nullptr) *provenance_out = schema.provenance;
        return schema;
    }
    return std::nullopt;
}

namespace {

EndpointSchema enrich_one(const DiscoveredEndpoint& endpoint,
                          const ResolvedBody* observed, const Document* document,
                          const SchemaGrabberOptions& options,
                          const Redactor& redactor) {
    EndpointSchema es;
    es.endpoint = endpoint;
    es.path_template = templatize_path(endpoint.path, &es.path_params);
    es.query = query_params_for(endpoint.url, redactor, options.redact_secrets);
    if (document != nullptr) {
        const std::string method = to_lower(endpoint.method);
        if (method == "post" || method == "put" || method == "patch" ||
            method == "delete") {
            es.form_fields = form_fields_for(*document, endpoint, options, redactor);
        }
    }
    std::string req_prov;
    es.request_schema =
        request_schema_for(endpoint, document, options, redactor, &req_prov);
    es.request_provenance = req_prov;

    // Response: observed bytes win; otherwise a heuristic generic object.
    if (observed != nullptr && !observed->body.empty()) {
        ResponseSchema rs;
        rs.status = observed->status != 0 ? observed->status : 200;
        rs.content_type = observed->content_type.empty()
                              ? endpoint.response_content_type
                              : observed->content_type;
        rs.observed = observed->status != 0;
        rs.provenance = rs.observed ? "observed-response" : "resolved-body";
        const std::string lower_ct = to_lower(rs.content_type);
        if (lower_ct.find("json") != std::string::npos ||
            (!observed->body.empty() &&
             (observed->body.front() == '{' || observed->body.front() == '['))) {
            std::string body = observed->body;
            if (body.size() > options.max_body_bytes)
                body.resize(options.max_body_bytes);
            auto schema =
                infer_json_schema(body, options, redactor, rs.provenance);
            if (schema.has_value()) rs.schema = std::move(schema);
        }
        es.responses.push_back(std::move(rs));
        es.response_provenance = es.responses.back().provenance;
    } else if (!endpoint.response_content_type.empty()) {
        ResponseSchema rs;
        rs.status = 200;
        rs.content_type = endpoint.response_content_type;
        rs.provenance = "content-type-hint";
        rs.observed = false;
        es.responses.push_back(std::move(rs));
        es.response_provenance = "content-type-hint";
    } else {
        ResponseSchema rs;
        rs.status = 200;
        rs.provenance = "heuristic-default";
        rs.observed = false;
        es.responses.push_back(std::move(rs));
        es.response_provenance = "heuristic-default";
    }
    return es;
}

void emit_field_yaml(std::ostringstream& out, const JsonField& f, int indent,
                     const SchemaGrabberOptions& options, const Redactor& redactor);

void emit_properties_yaml(std::ostringstream& out, const std::vector<JsonField>& props,
                          int indent, const SchemaGrabberOptions& options,
                          const Redactor& redactor) {
    const std::string pad(static_cast<std::size_t>(indent), ' ');
    if (props.empty()) {
        out << pad << "type: object\n";
        out << pad << "x-inferred: true\n";
        return;
    }
    out << pad << "type: object\n";
    out << pad << "properties:\n";
    for (const auto& f : props) emit_field_yaml(out, f, indent + 2, options, redactor);
    out << pad << "x-inferred: true\n";
}

void emit_field_yaml(std::ostringstream& out, const JsonField& f, int indent,
                     const SchemaGrabberOptions& options, const Redactor& redactor) {
    const std::string pad(static_cast<std::size_t>(indent), ' ');
    out << pad << yaml_quote(f.name) << ":\n";
    const std::string inner(static_cast<std::size_t>(indent + 2), ' ');
    if (f.type == "object") {
        out << inner << "type: object\n";
        if (!f.format.empty()) out << inner << "format: " << yaml_quote(f.format) << "\n";
        if (!f.properties.empty()) {
            out << inner << "properties:\n";
            for (const auto& sub : f.properties)
                emit_field_yaml(out, sub, indent + 4, options, redactor);
        }
    } else if (f.type == "array") {
        out << inner << "type: array\n";
        out << inner << "items:\n";
        const std::string item_pad(static_cast<std::size_t>(indent + 4), ' ');
        if (!f.items_properties.empty()) {
            out << item_pad << "type: object\n";
            out << item_pad << "properties:\n";
            for (const auto& sub : f.items_properties)
                emit_field_yaml(out, sub, indent + 6, options, redactor);
        } else {
            out << item_pad << "type: " << (f.items_type.empty() ? "string" : f.items_type)
                << "\n";
        }
    } else {
        out << inner << "type: " << f.type << "\n";
        if (!f.format.empty()) out << inner << "format: " << yaml_quote(f.format) << "\n";
    }
    if (options.include_examples && !f.example.empty()) {
        std::string example = f.example;
        if (options.redact_secrets && f.sensitive)
            example = redactor.policy().replacement;
        out << inner << "example: " << yaml_quote(example) << "\n";
    }
    (void)redactor;
}

std::string example_json_for(const InferredSchema& schema, std::size_t max_chars) {
    std::ostringstream out;
    out << "{";
    bool first = true;
    for (const auto& f : schema.properties) {
        if (!first) out << ", ";
        first = false;
        out << json_quote(f.name) << ": ";
        if (f.type == "integer" || f.type == "number" || f.type == "boolean") {
            out << (f.example.empty() ? (f.type == "boolean" ? "false" : "0") : f.example);
        } else if (f.type == "object") {
            out << "{}";
        } else if (f.type == "array") {
            out << "[]";
        } else {
            out << json_quote(f.example.empty() ? "string" : f.example);
        }
    }
    out << "}";
    std::string s = out.str();
    if (s.size() > max_chars) {
        s.resize(max_chars);
    }
    return s;
}

}  // namespace

std::vector<EndpointSchema> enrich_endpoints(
    const std::vector<DiscoveredEndpoint>& endpoints,
    const std::vector<ResolvedBody>& bodies, const Document* document,
    const SchemaGrabberOptions& options, const Redactor& redactor) {
    std::map<std::string, const ResolvedBody*> by_key;
    for (const auto& b : bodies) {
        by_key[b.endpoint.method + " " + b.endpoint.path] = &b;
    }
    std::map<std::string, EndpointSchema> merged;
    for (const auto& ep : endpoints) {
        const std::string key = ep.method + " " + ep.path;
        const ResolvedBody* observed = nullptr;
        const auto it = by_key.find(key);
        if (it != by_key.end()) observed = it->second;
        EndpointSchema es = enrich_one(ep, observed, document, options, redactor);
        // Merge duplicates by method+template: union query params and keep the
        // richer request/response schemas.
        const std::string mkey = es.endpoint.method + " " + es.path_template;
        auto mit = merged.find(mkey);
        if (mit == merged.end()) {
            merged.emplace(mkey, std::move(es));
            continue;
        }
        EndpointSchema& existing = mit->second;
        for (const auto& q : es.query) {
            bool known = false;
            for (auto& eq : existing.query) {
                if (eq.name == q.name) {
                    known = true;
                    if (eq.example.empty()) eq.example = q.example;
                    break;
                }
            }
            if (!known) existing.query.push_back(q);
        }
        std::sort(existing.query.begin(), existing.query.end(),
                  [](const QueryParam& a, const QueryParam& b) {
                      return a.name < b.name;
                  });
        if (!existing.request_schema.has_value() && es.request_schema.has_value()) {
            existing.request_schema = es.request_schema;
            existing.request_provenance = es.request_provenance;
        }
        if (existing.form_fields.empty() && !es.form_fields.empty())
            existing.form_fields = es.form_fields;
        for (const auto& r : es.responses) {
            bool known = false;
            for (const auto& er : existing.responses) {
                if (er.status == r.status && er.content_type == r.content_type) {
                    known = true;
                    break;
                }
            }
            if (!known) existing.responses.push_back(r);
        }
        if (es.endpoint.confidence > existing.endpoint.confidence) {
            existing.endpoint = es.endpoint;
            existing.endpoint.path = existing.path_template;
        }
    }
    std::vector<EndpointSchema> out;
    out.reserve(merged.size());
    for (auto& [k, v] : merged) out.push_back(std::move(v));
    std::sort(out.begin(), out.end(), [](const EndpointSchema& a, const EndpointSchema& b) {
        if (a.path_template != b.path_template) return a.path_template < b.path_template;
        return a.endpoint.method < b.endpoint.method;
    });
    return out;
}

SchemaGrabberResult grab_from_document_with_bodies(
    const Document& document, const std::vector<ResolvedBody>& bodies,
    const SchemaGrabberOptions& options, const Redactor& redactor) {
    EndpointExtractor extractor(to_extraction_opts(options));
    EndpointExtractionResult raw = extractor.extract(document);
    auto filtered = filter_to_api(raw.endpoints, options);

    SchemaGrabberResult result;
    result.warnings = raw.warnings;
    result.endpoints = filtered;
    result.schemas = enrich_endpoints(filtered, bodies, &document, options, redactor);
    result.openapi_yaml = render_schema_yaml(result.schemas, options, redactor);
    result.postman_json = render_schema_postman_json(result.schemas, options, redactor);
    return result;
}

SchemaGrabberResult grab_from_document(const Document& document,
                                       const SchemaGrabberOptions& options,
                                       const Redactor& redactor) {
    return grab_from_document_with_bodies(document, {}, options, redactor);
}

SchemaGrabberResult grab_from_session(Session& session,
                                      const SchemaGrabberOptions& options) {
    const Redactor redactor;
    auto document = session.document();
    if (document == nullptr) {
        SchemaGrabberResult empty;
        empty.warnings.push_back("session has no document");
        empty.openapi_yaml = render_schema_yaml({}, options, redactor);
        empty.postman_json = render_schema_postman_json({}, options, redactor);
        return empty;
    }
    EndpointExtractionOptions eo = to_extraction_opts(options);
    EndpointExtractor extractor(eo);
    for (const auto& call : session.page_script_requests()) {
        extractor.observe(call.method, call.url, call.status, call.content_type);
    }
    if (options.inspect_scripts) {
        for (const auto& text : session.page_script_texts()) {
            extractor.observe_script(text.url, text.body);
        }
    }
    EndpointExtractionResult raw = extractor.extract(*document);
    auto filtered = filter_to_api(raw.endpoints, options);

    // Bounded host-mediated GET probes for GET endpoints only. POST/PUT/PATCH
    // are never fired; their response schemas come from resolved bodies or
    // stay heuristic. Same-origin unless allow_cross_origin.
    std::vector<ResolvedBody> bodies;
    std::uint32_t probes = 0;
    if (options.probe_get_responses) {
        std::string base = document->base_url().empty() ? document->url()
                                                        : document->base_url();
        if (base.empty()) base = session.current_url();
        const std::string seed_origin = origin_of(base);
        std::set<std::string> visited;
        for (const auto& ep : filtered) {
            if (probes >= options.max_probe_requests) break;
            if (to_lower(ep.method) != "get") continue;
            std::string fetch_url = ep.url;
            try {
                if (!base.empty() && fetch_url.rfind("http://", 0) != 0 &&
                    fetch_url.rfind("https://", 0) != 0) {
                    fetch_url = resolve_url(base, fetch_url);
                }
            } catch (...) {
                continue;
            }
            if (!options.allow_cross_origin && !seed_origin.empty() &&
                !same_origin(base, fetch_url))
                continue;
            if (!visited.insert(fetch_url).second) continue;
            try {
                HttpRequest req;
                req.method = "GET";
                req.url = fetch_url;
                HttpResponse resp = session.request(req);
                ResolvedBody rb;
                rb.endpoint = ep;
                rb.endpoint.url = fetch_url;
                rb.final_url = resp.final_url.empty() ? fetch_url : resp.final_url;
                rb.status = resp.status;
                rb.body = resp.body.size() > options.max_body_bytes
                              ? resp.body.substr(0, options.max_body_bytes)
                              : resp.body;
                rb.content_type = resp.header("Content-Type");
                bodies.push_back(std::move(rb));
                ++probes;
            } catch (...) {
                ++probes;
            }
        }
    }

    SchemaGrabberResult result;
    result.warnings = raw.warnings;
    result.endpoints = filtered;
    result.probe_count = probes;
    result.schemas = enrich_endpoints(filtered, bodies, document.get(), options, redactor);
    if (options.probe_get_responses && probes >= options.max_probe_requests &&
        !filtered.empty()) {
        result.warnings.push_back(
            "probe budget exhausted; some response schemas stay heuristic");
    }
    result.openapi_yaml = render_schema_yaml(result.schemas, options, redactor);
    result.postman_json = render_schema_postman_json(result.schemas, options, redactor);
    return result;
}

SchemaGrabberResult grab(Session& session, const SchemaGrabberOptions& options) {
    if (!options.html.empty()) {
        std::string base = options.base_url.empty() ? options.url : options.base_url;
        if (base.empty()) base = "https://example.com/";
        session.load_html(options.html, base);
        return grab_from_session(session, options);
    }
    if (!options.url.empty()) {
        try {
            session.navigate(options.url);
        } catch (...) {
            if (session.document() == nullptr) throw;
        }
    }
    return grab_from_session(session, options);
}

std::string render_schema_yaml(const std::vector<EndpointSchema>& schemas,
                               const SchemaGrabberOptions& options,
                               const Redactor& redactor) {
    std::ostringstream out;
    out << "openapi: " << options.openapi_version << "\n";
    out << "info:\n";
    out << "  title: 'Discovered API (schema-grabber)'\n";
    out << "  version: '0.1.0'\n";
    out << "  description: >-\n";
    out << "    Generated by ProwseTk schema-grabber. Endpoint request/response\n";
    out << "    schemas and URL parameters are heuristic reverse-engineering;\n";
    out << "    not authoritative API documentation. Inferred values are marked\n";
    out << "    and carry provenance and confidence. Use alongside\n";
    out << "    scrape-endpoints for discovery plus schema enrichment.\n";
    out << "x-prowsetk-generated: true\n";
    out << "x-prowsetk-plugin: schema-grabber\n";

    std::map<std::string, std::vector<const EndpointSchema*>> by_path;
    for (const auto& s : schemas) {
        std::string p = s.path_template.empty() ? "/" : s.path_template;
        by_path[p].push_back(&s);
    }
    out << "paths:";
    if (by_path.empty()) {
        out << " {}\n";
        return out.str();
    }
    out << "\n";
    for (const auto& [path, entries] : by_path) {
        out << "  " << yaml_quote(path) << ":\n";
        std::vector<const EndpointSchema*> sorted = entries;
        std::sort(sorted.begin(), sorted.end(), [](const auto* a, const auto* b) {
            return a->endpoint.method < b->endpoint.method;
        });
        for (const auto* es : sorted) {
            const auto& ep = es->endpoint;
            out << "    " << ep.method << ":\n";
            out << "      operationId: " << operation_id(ep.method, es->path_template)
                << "\n";
            if (!es->path_params.empty() || !es->query.empty()) {
                out << "      parameters:\n";
                for (const auto& pp : es->path_params) {
                    out << "        - name: " << yaml_quote(pp.name) << "\n";
                    out << "          in: path\n";
                    out << "          required: true\n";
                    out << "          schema:\n";
                    out << "            type: " << pp.type << "\n";
                    if (options.include_examples && !pp.example.empty())
                        out << "            example: " << yaml_quote(pp.example) << "\n";
                    out << "          x-inferred: true\n";
                }
                for (const auto& qp : es->query) {
                    out << "        - name: " << yaml_quote(qp.name) << "\n";
                    out << "          in: query\n";
                    out << "          required: false\n";
                    out << "          schema:\n";
                    out << "            type: " << qp.type << "\n";
                    if (options.include_examples && !qp.example.empty()) {
                        std::string ex = qp.example;
                        if (options.redact_secrets && qp.sensitive)
                            ex = redactor.policy().replacement;
                        out << "            example: " << yaml_quote(ex) << "\n";
                    }
                    out << "          x-inferred: true\n";
                }
            }
            // Request body for methods that carry one, or whenever a schema
            // was inferred (form fields / script hints).
            std::string req_ct = ep.request_content_type;
            if (req_ct.empty() && es->request_schema.has_value()) {
                const std::string m = to_lower(ep.method);
                if (m == "post" || m == "put" || m == "patch")
                    req_ct = "application/json";
                else if (!es->form_fields.empty())
                    req_ct = "application/x-www-form-urlencoded";
            }
            if (es->request_schema.has_value() && !req_ct.empty()) {
                out << "      requestBody:\n";
                out << "        content:\n";
                out << "          " << yaml_quote(req_ct) << ":\n";
                out << "            schema:\n";
                emit_properties_yaml(out, es->request_schema->properties, 14,
                                     options, redactor);
                if (!es->request_schema->required.empty()) {
                    out << "              required:\n";
                    for (const auto& r : es->request_schema->required)
                        out << "                - " << yaml_quote(r) << "\n";
                }
            } else if (!req_ct.empty()) {
                out << "      requestBody:\n";
                out << "        content:\n";
                out << "          " << yaml_quote(req_ct) << ":\n";
                out << "            schema:\n";
                out << "              type: object\n";
                out << "              x-inferred: true\n";
            }
            // Responses: one entry per observed/inferred response.
            out << "      responses:\n";
            if (es->responses.empty()) {
                out << "        '200':\n";
                out << "          description: Inferred response\n";
            }
            for (const auto& rs : es->responses) {
                out << "        '" << rs.status << "':\n";
                out << "          description: "
                    << (rs.observed ? "Observed response" : "Inferred response") << "\n";
                if (!rs.content_type.empty()) {
                    out << "          content:\n";
                    out << "            " << yaml_quote(rs.content_type) << ":\n";
                    out << "              schema:\n";
                    if (rs.schema.has_value()) {
                        if (rs.schema->type == "array") {
                            out << "                type: array\n";
                            out << "                items:\n";
                            if (!rs.schema->properties.empty()) {
                                out << "                  type: object\n";
                                out << "                  properties:\n";
                                for (const auto& f : rs.schema->properties)
                                    emit_field_yaml(out, f, 18, options, redactor);
                            } else {
                                out << "                  type: object\n";
                            }
                            out << "                x-inferred: true\n";
                        } else {
                            emit_properties_yaml(out, rs.schema->properties, 16,
                                                 options, redactor);
                        }
                    } else {
                        out << "                type: object\n";
                        out << "                x-inferred: true\n";
                    }
                }
            }
            if (options.infer_schemas) out << "      x-inferred: true\n";
            // Schema provenance extension: where req/res shapes came from.
            out << "      x-prowsetk-schema:\n";
            out << "        path-template: " << yaml_quote(es->path_template) << "\n";
            if (!es->request_provenance.empty())
                out << "        request-provenance: "
                    << yaml_quote(es->request_provenance) << "\n";
            if (!es->response_provenance.empty())
                out << "        response-provenance: "
                    << yaml_quote(es->response_provenance) << "\n";
            out << "        inferred: true\n";
            if (options.include_provenance) {
                std::string source = options.redact_secrets
                                         ? redactor.redact_url(ep.source)
                                         : ep.source;
                std::string url = options.redact_secrets
                                      ? redactor.redact_url(ep.url)
                                      : ep.url;
                out << "      x-prowsetk-provenance:\n";
                out << "        source: " << yaml_quote(source) << "\n";
                out << "        url: " << yaml_quote(url) << "\n";
                out << "        discovery-method: " << yaml_quote(ep.discovery_method)
                    << "\n";
                out << "        confidence: " << format_conf(ep.confidence) << "\n";
                out << "        inferred: true\n";
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

namespace {

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
            query += redactor.is_sensitive_query_parameter(decode_component(key))
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

// Builds a Postman URL: path template `{id}` -> `:id`, query params appended
// with example values (redacted when sensitive).
std::string postman_request_url(const EndpointSchema& es,
                                const SchemaGrabberOptions& options,
                                const Redactor& redactor) {
    std::string path = es.path_template.empty() ? "/" : es.path_template;
    // Replace {name} with :name.
    std::string converted;
    for (std::size_t i = 0; i < path.size(); ++i) {
        if (path[i] == '{') {
            const auto close = path.find('}', i);
            if (close != std::string::npos) {
                converted += ':';
                converted += path.substr(i + 1, close - i - 1);
                i = close;
                continue;
            }
        }
        converted += path[i];
    }
    std::string base;
    try {
        Url parsed = parse_url(es.endpoint.url);
        base = parsed.scheme + "://" + parsed.host;
        if (!parsed.port.empty()) base += ":" + parsed.port;
    } catch (...) {
        base = "";
    }
    std::string url = base.empty() ? converted : base + converted;
    // Collect existing query keys so inferred params are appended once.
    std::set<std::string> known;
    try {
        const std::string q = parse_url(es.endpoint.url).query;
        for (std::size_t start = 0; start < q.size();) {
            const auto end = q.find('&', start);
            const auto pair =
                q.substr(start, end == std::string::npos ? end : end - start);
            known.insert(decode_component(pair.substr(0, pair.find('='))));
            if (end == std::string::npos) break;
            start = end + 1;
        }
    } catch (...) {
    }
    constexpr char hex[] = "0123456789ABCDEF";
    auto enc = [&](const std::string& s) {
        std::string o;
        for (unsigned char c : s) {
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
                c == '~') {
                o += static_cast<char>(c);
            } else {
                o += '%';
                o += hex[c >> 4];
                o += hex[c & 15];
            }
        }
        return o;
    };
    // Seed from the live query string first so examples survive.
    try {
        const std::string q = parse_url(es.endpoint.url).query;
        for (std::size_t start = 0; start < q.size();) {
            const auto end = q.find('&', start);
            const auto pair =
                q.substr(start, end == std::string::npos ? end : end - start);
            const auto eq = pair.find('=');
            const std::string key = pair.substr(0, eq);
            const std::string decoded = decode_component(key);
            std::string value = eq == std::string::npos ? "" : pair.substr(eq + 1);
            // Replace sensitive values with the redaction replacement.
            for (const auto& qp : es.query) {
                if (qp.name == decoded && qp.sensitive && options.redact_secrets) {
                    value = redactor.policy().replacement;
                    break;
                }
            }
            if (!url.empty()) {
                url += (url.find('?') == std::string::npos ? "?" : "&");
                url += key + "=" + value;
            }
            if (end == std::string::npos) break;
            start = end + 1;
        }
    } catch (...) {
    }
    for (const auto& qp : es.query) {
        if (known.find(qp.name) != known.end()) continue;
        known.insert(qp.name);
        url += (url.find('?') == std::string::npos ? "?" : "&");
        url += enc(qp.name) + "=";
        std::string ex = qp.example;
        if (qp.sensitive && options.redact_secrets) ex = redactor.policy().replacement;
        url += enc(ex);
    }
    return url;
}

}  // namespace

std::string render_schema_postman_json(const std::vector<EndpointSchema>& schemas,
                                       const SchemaGrabberOptions& options,
                                       const Redactor& redactor) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    std::string description =
        "Generated by ProwseTk schema-grabber. Heuristic request/response schemas "
        "and URL parameters; not authoritative documentation. Secrets are redacted.";
    out << "{\n  \"info\": {\"name\": " << json_quote(options.collection_name)
        << ", \"schema\": "
           "\"https://schema.getpostman.com/json/collection/v2.1.0/collection.json\","
        << " \"description\": " << json_quote(description) << "},\n  \"item\": [";

    std::map<std::pair<std::string, std::string>, const EndpointSchema*> ordered;
    for (const auto& s : schemas) {
        ordered.try_emplace(
            std::make_pair(s.path_template, postman_method_name(s.endpoint.method)),
            &s);
    }
    bool first = true;
    for (const auto& [key, es] : ordered) {
        const auto& ep = es->endpoint;
        const auto url = safe_postman_url(postman_request_url(*es, options, redactor),
                                          options.redact_secrets, redactor);
        const auto& method = key.second;
        if (!first) out << ',';
        first = false;
        std::ostringstream item_description;
        item_description.imbue(std::locale::classic());
        item_description << "Heuristic discovery; inferred schemas. Confidence: "
                         << (std::isfinite(ep.confidence)
                                 ? std::clamp(ep.confidence, 0.0, 1.0)
                                 : 0.0);
        if (options.include_provenance) {
            item_description << "\nSource: "
                             << safe_postman_url(ep.source, options.redact_secrets,
                                                 redactor)
                             << "\nDiscovery method: " << ep.discovery_method;
            if (!es->request_provenance.empty())
                item_description << "\nRequest schema: " << es->request_provenance;
            if (!es->response_provenance.empty())
                item_description << "\nResponse schema: " << es->response_provenance;
        }
        out << "\n    {\"name\": " << json_quote(method + " " + url)
            << ", \"request\": {\"method\": " << json_quote(method)
            << ", \"url\": " << json_quote(url)
            << ", \"description\": " << json_quote(item_description.str())
            << ", \"header\": [";
        std::string req_ct = ep.request_content_type;
        if (req_ct.empty() && es->request_schema.has_value()) {
            const std::string m = to_lower(ep.method);
            if (m == "post" || m == "put" || m == "patch") req_ct = "application/json";
        }
        if (!req_ct.empty()) {
            out << "{\"key\": \"Content-Type\", \"value\": " << json_quote(req_ct)
                << '}';
        }
        out << ']';
        // Query variables: Postman "url" stays a string here, but expose the
        // typed query params as well-formed query pairs already in the URL.
        // Bodies carry the reverse-engineered schema.
        if (es->request_schema.has_value() && !req_ct.empty()) {
            if (req_ct == "application/x-www-form-urlencoded") {
                out << ", \"body\": {\"mode\": \"urlencoded\", \"urlencoded\": [";
                bool first_field = true;
                for (const auto& f : es->request_schema->properties) {
                    if (!first_field) out << ',';
                    first_field = false;
                    std::string val = f.example;
                    if (f.sensitive && options.redact_secrets)
                        val = redactor.policy().replacement;
                    out << "{\"key\": " << json_quote(f.name) << ", \"value\": "
                        << json_quote(val) << ", \"type\": " << json_quote(f.type)
                        << ", \"description\": \"Inferred field (" << f.provenance
                        << "); supply a value\"}";
                }
                out << "]}";
            } else {
                out << ", \"body\": {\"mode\": \"raw\", \"raw\": "
                    << json_quote(example_json_for(*es->request_schema,
                                                  options.max_example_chars * 8))
                    << ", \"options\": {\"raw\": {\"language\": \"json\"}}}";
            }
        }
        // Path variables.
        if (!es->path_params.empty()) {
            out << ", \"variable\": [";
            bool first_var = true;
            for (const auto& pp : es->path_params) {
                if (!first_var) out << ',';
                first_var = false;
                out << "{\"key\": " << json_quote(pp.name) << ", \"value\": "
                    << json_quote(pp.example.empty() ? "1" : pp.example)
                    << ", \"description\": \"Inferred path parameter\"}";
            }
            out << ']';
        }
        out << "}, \"response\": [";
        bool first_resp = true;
        for (const auto& rs : es->responses) {
            if (!rs.schema.has_value() && rs.content_type.empty() && !rs.observed)
                continue;
            if (!first_resp) out << ',';
            first_resp = false;
            std::string body_example;
            if (rs.schema.has_value())
                body_example = example_json_for(*rs.schema, options.max_example_chars * 8);
            out << "{\"name\": " << json_quote("Example " + std::to_string(rs.status))
                << ", \"status\": " << json_quote(std::to_string(rs.status))
                << ", \"code\": " << rs.status << ", \"header\": [";
            if (!rs.content_type.empty())
                out << "{\"key\": \"Content-Type\", \"value\": "
                    << json_quote(rs.content_type) << '}';
            out << "], \"body\": " << json_quote(body_example) << "}";
        }
        out << "]}";
    }
    out << "\n  ]\n}\n";
    return out.str();
}

void SchemaGrabberResult::write_openapi_yaml(
    const std::filesystem::path& path) const {
    std::ofstream file;
    file.exceptions(std::ios::failbit | std::ios::badbit);
    file.open(path, std::ios::binary | std::ios::trunc);
    file << openapi_yaml;
    file.close();
}

void SchemaGrabberResult::write_postman_json(
    const std::filesystem::path& path) const {
    std::ofstream file;
    file.exceptions(std::ios::failbit | std::ios::badbit);
    file.open(path, std::ios::binary | std::ios::trunc);
    file << postman_json;
    file.close();
}

}  // namespace prowsetk::plugins::schema_grabber
