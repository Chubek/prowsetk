#include "prowsetk/pdql.hpp"
#include "prowsetk/pdql.h"
#include "prowsetk/document.hpp"
#include "prowsetk/error.hpp"
#include "prowsetk/redaction.hpp"
#include "prowsetk/xpath.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <stdexcept>

namespace prowsetk::pdql {
namespace {
std::string trim(std::string_view s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == s.npos) return {};
    return std::string(s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1));
}
std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string quote(std::string_view s) {
    constexpr char hex[] = "0123456789abcdef";
    std::string out = "\"";
    for (unsigned char c : s) {
        if (c == '\\' || c == '"') { out += '\\'; out += static_cast<char>(c); }
        else if (c < 32) { out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15]; }
        else out += static_cast<char>(c);
    }
    return out + '"';
}
std::string xml(std::string_view s) {
    std::string out;
    for (char c : s) {
        if (c == '&') out += "&amp;";
        else if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else if (c == '"') out += "&quot;";
        else out += c;
    }
    return out;
}
bool glob(std::string_view p, std::string_view v) {
    std::size_t a = 0, b = 0, star = p.npos, mark = 0;
    while (b < v.size()) {
        if (a < p.size() && (p[a] == '?' || p[a] == v[b])) { ++a; ++b; }
        else if (a < p.size() && p[a] == '*') { star = a++; mark = b; }
        else if (star != p.npos) { a = star + 1; b = ++mark; }
        else return false;
    }
    while (a < p.size() && p[a] == '*') ++a;
    return a == p.size();
}
// Split at top-level delimiters only: attr("data,a") remains one expression.
std::vector<std::string> split(std::string_view s) {
    std::vector<std::string> out;
    std::size_t start = 0;
    int depth = 0;
    char quoted = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (quoted) { if (c == quoted && (i == 0 || s[i - 1] != '\\')) quoted = 0; }
        else if (c == '"' || c == '\'') quoted = c;
        else if (c == '(') ++depth;
        else if (c == ')') --depth;
        else if (c == ',' && depth == 0) { out.push_back(trim(s.substr(start, i - start))); start = i + 1; }
    }
    if (quoted || depth != 0) throw Error(ErrorCode::ParseError, "unbalanced PDQL projection");
    out.push_back(trim(s.substr(start)));
    return out;
}
std::string unquote(std::string s) {
    if (s.size() < 2 || (s.front() != '"' && s.front() != '\'') || s.back() != s.front())
        throw Error(ErrorCode::ParseError, "PDQL requires a quoted string");
    return s.substr(1, s.size() - 2);
}
bool sensitive(std::string name) {
    name = lower(std::move(name));
    const Redactor redactor;
    if (redactor.is_sensitive_header(name) || redactor.is_sensitive_query_parameter(name)) return true;
    for (const auto* part : {"password", "token", "secret", "cookie", "authorization", "api-key", "api_key"})
        if (name.find(part) != name.npos) return true;
    return false;
}
void sanitize(::prowsetk::Document& doc) {
    const Redactor redactor;
    for (const auto& element : doc.query_selector_all("*")) {
        const auto tag = element->tag_name();
        if (tag == "script" || tag == "style" || tag == "textarea" || tag == "select") element->set_text("");
        for (const auto& attr : element->attributes()) {
            if (sensitive(attr.name) || attr.name.starts_with("on") ||
                ((tag == "input" || tag == "option" || tag == "button") && attr.name == "value")) {
                element->remove_attribute(attr.name);
            } else if (attr.name == "href" || attr.name == "src" || attr.name == "action") {
                element->set_attribute(attr.name, redactor.redact_url(attr.value));
            }
        }
    }
}
std::unique_ptr<Node> mirror(const std::shared_ptr<Element>& element, Node* parent, std::size_t& bytes) {
    auto node = std::make_unique<Node>();
    node->parent = parent;
    node->tag = element->tag_name();
    node->text = element->text();
    bytes += node->text.size();
    if (bytes > 16 * 1024 * 1024) throw Error(ErrorCode::ResourceLimit, "PDQL tree text exceeds 16 MiB");
    for (const auto& attr : element->attributes()) node->attributes.emplace(attr.name, attr.value);
    for (const auto& child : element->children()) node->children.push_back(mirror(child, node.get(), bytes));
    return node;
}
std::string property(const Element& node, std::string expr) {
    if (expr.starts_with("node.")) expr.erase(0, 5);
    bool trimmed = false;
    if (expr.ends_with(".trim()")) { trimmed = true; expr.resize(expr.size() - 7); }
    std::string value;
    if (expr == "text") value = node.text();
    else if (expr == "tag" || expr == "tag_name") value = node.tag_name();
    else if (expr.starts_with("attr(") && expr.ends_with(')')) value = node.attribute(unquote(trim(std::string_view(expr).substr(5, expr.size() - 6))));
    else throw Error(ErrorCode::ParseError, "unsupported PDQL expression");
    return trimmed ? trim(value) : value;
}
double number(const std::string& text) {
    char* end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    if (end == text.c_str() || !std::isfinite(value)) return 0;
    return value;
}
std::string xpath_source(std::string_view selector) {
    return unquote(trim(selector.substr(6, selector.size() - 7)));
}
}

Document Document::parse(std::string_view html) {
    Document result;
    result.source_ = parse_html(html);
    std::size_t bytes = 0;
    result.root_ = mirror(result.source_->root(), nullptr, bytes);
    return result;
}

Query::Query(std::string_view source) {
    if (source.size() > 65536) throw Error(ErrorCode::ResourceLimit, "PDQL query exceeds 64 KiB");
    std::string folded;
    char quoted = 0;
    for (std::size_t i = 0; i < source.size(); ++i) {
        const char c = source[i];
        if (quoted) {
            folded += c;
            if (c == quoted && (i == 0 || source[i - 1] != '\\')) quoted = 0;
        } else if (c == '\'' || c == '"') { quoted = c; folded += c; }
        else if (std::isspace(static_cast<unsigned char>(c))) { if (!folded.empty() && folded.back() != ' ') folded += ' '; }
        else folded += c;
    }
    if (quoted) throw Error(ErrorCode::ParseError, "unterminated PDQL string");
    auto text = trim(folded);
    if (text.starts_with("query ") || text.starts_with("query{")) {
        const auto brace = text.find('{');
        if (brace == text.npos || !text.ends_with('}')) throw Error(ErrorCode::ParseError, "invalid PDQL query block");
        text = trim(std::string_view(text).substr(brace + 1, text.size() - brace - 2));
    }
    const auto serialization = text.rfind(" serialize as ");
    if (serialization != text.npos) text.resize(serialization);
    if (text.starts_with("from ")) {
        const auto select = text.find(" select ");
        if (select == text.npos) throw Error(ErrorCode::ParseError, "PDQL from requires select");
        selector_ = trim(std::string_view(text).substr(5, select - 5));
        fields_ = trim(std::string_view(text).substr(select + 8));
        if (fields_.starts_with('{') && fields_.ends_with('}')) fields_ = trim(std::string_view(fields_).substr(1, fields_.size() - 2));
        const auto where = selector_.find(" where ");
        if (where != selector_.npos) { where_ = trim(std::string_view(selector_).substr(where + 7)); selector_.resize(where); }
    } else if (text.starts_with("select ")) {
        const auto from = text.find(" from ");
        if (from == text.npos) throw Error(ErrorCode::ParseError, "PDQL select requires from");
        fields_ = trim(std::string_view(text).substr(7, from - 7));
        selector_ = trim(std::string_view(text).substr(from + 6));
        const auto where = selector_.find(" where ");
        if (where != selector_.npos) { where_ = trim(std::string_view(selector_).substr(where + 7)); selector_.resize(where); }
    } else { selector_ = text; fields_ = "text"; }
    if (selector_.empty() || fields_.empty()) throw Error(ErrorCode::ParseError, "empty PDQL selector or projection");
    if (selector_.starts_with("xpath(")) {
        if (!selector_.ends_with(')')) throw Error(ErrorCode::ParseError, "invalid PDQL XPath selector");
        evaluate_xpath(*parse_html("<probe>text</probe>"), xpath_source(selector_));
    } else {
        if (selector_.starts_with('<')) {
            if (!selector_.ends_with('>')) throw Error(ErrorCode::ParseError, "invalid PDQL tag delimiter");
            selector_ = selector_.substr(1, selector_.size() - 2);
        }
        if (selector_.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_*?") != selector_.npos)
            throw Error(ErrorCode::ParseError, "unsupported PDQL tag selector");
    }
    const auto probe = parse_html("<probe>1</probe>")->query_selector("probe");
    for (auto field : split(fields_)) {
        const auto colon = field.find(':');
        const auto expr = colon == field.npos ? field : trim(std::string_view(field).substr(colon + 1));
        if (expr == "count()" || expr == "count" || expr == "sum()" || expr == "sum" || expr == "avg()" || expr == "avg" || expr == "number") continue;
        if (expr.starts_with("number(") && expr.ends_with(')')) property(*probe, trim(std::string_view(expr).substr(7, expr.size() - 8)));
        else property(*probe, expr);
    }
    if (!where_.empty()) {
        const auto equal = where_.find('=');
        if (equal == where_.npos) throw Error(ErrorCode::ParseError, "unsupported PDQL guard");
        property(*probe, trim(std::string_view(where_).substr(0, equal)));
        unquote(trim(std::string_view(where_).substr(equal + (where_.substr(equal, 2) == "==" ? 2 : 1))));
    }
}

std::vector<Row> Query::execute(const Document& document) const {
    if (!document.source_) throw Error(ErrorCode::InvalidArgument, "PDQL document is not initialized");
    return execute(*document.source_, false);
}
std::vector<Row> Query::execute(const ::prowsetk::Document& document, bool redact) const {
    if (!document.valid()) throw Error(ErrorCode::InvalidArgument, "PDQL requires a valid DOM");
    auto snapshot = parse_html(document.html());
    if (redact) sanitize(*snapshot);
    std::vector<std::shared_ptr<Element>> matches;
    if (selector_.starts_with("xpath(")) {
        const auto value = evaluate_xpath(*snapshot, xpath_source(selector_));
        if (value.type != XPathValueType::NodeSet) throw Error(ErrorCode::ParseError, "PDQL XPath must select nodes");
        matches = value.nodes;
    } else {
        for (const auto& node : snapshot->query_selector_all("*"))
            if (glob(lower(selector_), node->tag_name())) matches.push_back(node);
    }
    if (!where_.empty()) {
        const auto equal = where_.find('=');
        const auto key = trim(std::string_view(where_).substr(0, equal));
        const auto expected = unquote(trim(std::string_view(where_).substr(equal + (where_.substr(equal, 2) == "==" ? 2 : 1))));
        std::erase_if(matches, [&](const auto& node) { return !node || property(*node, key) != expected; });
    }
    if (matches.size() > 10000) throw Error(ErrorCode::ResourceLimit, "PDQL exceeds 10000 rows");
    std::vector<Row> rows;
    std::size_t projected_bytes = 0;
    double sum = 0;
    for (const auto& node : matches) if (node) sum += number(node->text());
    for (const auto& node : matches) {
        if (!node) continue;
        Row row;
        for (auto field : split(fields_)) {
            const auto colon = field.find(':');
            auto name = colon == field.npos ? field : trim(std::string_view(field).substr(0, colon));
            auto expr = colon == field.npos ? field : trim(std::string_view(field).substr(colon + 1));
            Value value;
            if (expr == "count" || expr == "count()") value = {std::to_string(matches.size()), true};
            else if (expr == "sum" || expr == "sum()" || expr == "avg" || expr == "avg()") value = {std::to_string(expr.starts_with("avg") ? sum / static_cast<double>(matches.size()) : sum), true};
            else if (expr == "number") value = {std::to_string(number(node->text())), true};
            else if (expr.starts_with("number(") && expr.ends_with(')')) value = {std::to_string(number(property(*node, trim(std::string_view(expr).substr(7, expr.size() - 8))))), true};
            else {
                value.text = property(*node, expr);
                if (colon == field.npos && expr.starts_with("attr(")) name = unquote(trim(std::string_view(expr).substr(5, expr.size() - 6)));
            }
            projected_bytes += name.size() + value.text.size();
            if (projected_bytes > 16 * 1024 * 1024) throw Error(ErrorCode::ResourceLimit, "PDQL projection exceeds 16 MiB");
            row.fields.emplace(std::move(name), std::move(value));
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

std::string serialize(const std::vector<Row>& rows, Format format) {
    if (format < Format::Json || format > Format::SExpr) throw Error(ErrorCode::InvalidArgument, "invalid PDQL format");
    std::ostringstream out;
    if (format == Format::Json || format == Format::SExpr) out << (format == Format::Json ? "[" : "(");
    else if (format == Format::Xml) out << "<results>";
    if (format == Format::Yaml && rows.empty()) return "[]\n";
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (format == Format::Json) out << (i ? ",{": "{");
        else if (format == Format::Yaml) out << "-";
        else if (format == Format::Xml) out << "<row>";
        else out << "(";
        std::size_t j = 0;
        for (const auto& [name, value] : rows[i].fields) {
            const auto encoded = value.numeric ? value.text : quote(value.text);
            if (format == Format::Json) out << (j++ ? "," : "") << quote(name) << ":" << encoded;
            else if (format == Format::Yaml) out << "\n  " << quote(name) << ": " << encoded;
            else if (format == Format::Xml) out << "<field name=\"" << xml(name) << "\">" << xml(value.text) << "</field>";
            else out << "(" << quote(name) << " " << encoded << ")";
        }
        if (format == Format::Json) out << "}";
        else if (format == Format::Yaml) out << "\n";
        else if (format == Format::Xml) out << "</row>";
        else out << ")";
        if (out.tellp() > 16 * 1024 * 1024) throw Error(ErrorCode::ResourceLimit, "PDQL serialization exceeds 16 MiB");
    }
    if (format == Format::Json) out << "]";
    else if (format == Format::Xml) out << "</results>";
    else if (format == Format::SExpr) out << ")";
    return out.str();
}
}

struct pt_pdql_document { prowsetk::pdql::Document value; };
struct pt_pdql_query { prowsetk::pdql::Query value; };
struct pt_pdql_result { std::vector<prowsetk::pdql::Row> value; };
namespace {
char* copy_string(const char* value) {
    auto* result = static_cast<char*>(std::malloc(std::strlen(value) + 1));
    if (result) std::strcpy(result, value);
    return result;
}
template<class F> auto boundary(char** error, F function) -> decltype(function()) {
    if (error) *error = nullptr;
    try { return function(); }
    catch (const std::exception& e) { if (error) *error = copy_string(e.what()); }
    catch (...) { if (error) *error = copy_string("PDQL failure"); }
    return nullptr;
}
const prowsetk::pdql::Node* node(const pt_pdql_node* n) { return reinterpret_cast<const prowsetk::pdql::Node*>(n); }
}
extern "C" {
pt_pdql_document* pt_pdql_document_create(const char* html, char** error) {
    return boundary(error, [&]() { if (!html) throw std::invalid_argument("null HTML"); return std::make_unique<pt_pdql_document>(pt_pdql_document{prowsetk::pdql::Document::parse(html)}).release(); });
}
void pt_pdql_document_free(pt_pdql_document* d) { std::unique_ptr<pt_pdql_document> owned(d); }
pt_pdql_node* pt_pdql_document_root(pt_pdql_document* d) { return d ? reinterpret_cast<pt_pdql_node*>(const_cast<prowsetk::pdql::Node*>(&d->value.root())) : nullptr; }
size_t pt_pdql_node_child_count(const pt_pdql_node* n) { return n ? node(n)->children.size() : 0; }
pt_pdql_node* pt_pdql_node_child(const pt_pdql_node* n, size_t i) { return n && i < node(n)->children.size() ? reinterpret_cast<pt_pdql_node*>(node(n)->children[i].get()) : nullptr; }
const char* pt_pdql_node_tag(const pt_pdql_node* n) { return n ? node(n)->tag.c_str() : nullptr; }
const char* pt_pdql_node_text(const pt_pdql_node* n) { return n ? node(n)->text.c_str() : nullptr; }
const char* pt_pdql_node_attribute(const pt_pdql_node* n, const char* key) {
    if (!n || !key) return nullptr;
    return boundary(nullptr, [&]() -> const char* { std::string name(key); for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); const auto it = node(n)->attributes.find(name); return it == node(n)->attributes.end() ? nullptr : it->second.c_str(); });
}
pt_pdql_query* pt_pdql_query_compile(const char* source, char** error) {
    return boundary(error, [&]() { if (!source) throw std::invalid_argument("null query"); return std::make_unique<pt_pdql_query>(pt_pdql_query{prowsetk::pdql::Query(source)}).release(); });
}
void pt_pdql_query_free(pt_pdql_query* q) { std::unique_ptr<pt_pdql_query> owned(q); }
pt_pdql_result* pt_pdql_query_execute(const pt_pdql_query* q, const pt_pdql_document* d, char** error) {
    return boundary(error, [&]() { if (!q || !d) throw std::invalid_argument("null query or document"); return std::make_unique<pt_pdql_result>(pt_pdql_result{q->value.execute(d->value)}).release(); });
}
char* pt_pdql_result_serialize(const pt_pdql_result* r, pt_pdql_format format, char** error) {
    return boundary(error, [&]() { if (!r || format < PT_PDQL_JSON || format > PT_PDQL_SEXPR) throw std::invalid_argument("invalid result or format"); const auto value = prowsetk::pdql::serialize(r->value, static_cast<prowsetk::pdql::Format>(format)); auto* result = copy_string(value.c_str()); if (!result) throw std::bad_alloc(); return result; });
}
void pt_pdql_result_free(pt_pdql_result* r) { std::unique_ptr<pt_pdql_result> owned(r); }
void pt_pdql_str_free(char* s) { std::free(s); }
}
