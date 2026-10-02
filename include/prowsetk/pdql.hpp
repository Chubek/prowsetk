#ifndef PROWSETK_PDQL_HPP
#define PROWSETK_PDQL_HPP
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
namespace prowsetk { class Document; }
namespace prowsetk::pdql {
enum class Format { Json, Yaml, Xml, SExpr };
struct Node {
    std::string tag, text;
    std::map<std::string, std::string> attributes;
    std::vector<std::unique_ptr<Node>> children;
    Node* parent = nullptr; // Borrowed; the document owns the entire tree.
};
class Document {
public:
    // Uses Flatworm's bounded HTML parser; the C-node text mirror is also
    // bounded to 16 MiB. Parsing does not execute JavaScript.
    static Document parse(std::string_view html);
    const Node& root() const { return *root_; }
private:
    std::unique_ptr<Node> root_;
    std::shared_ptr<::prowsetk::Document> source_;
    friend class Query;
};
struct Value { std::string text; bool numeric = false; };
struct Row { std::map<std::string, Value> fields; };
class Query {
public:
    // Compilation validates the supported syntax and XPath. Query source is
    // bounded to 64 KiB, execution to 10,000 rows / 16 MiB of projected data.
    explicit Query(std::string_view source);
    std::vector<Row> execute(const Document& document) const;
    // Queries a snapshot of the installed Flatworm DOM, including script/host
    // mutations. Default redaction removes scripts, private control values,
    // sensitive attributes and sensitive URL parameters before selection.
    std::vector<Row> execute(const ::prowsetk::Document& document,
                             bool redact = true) const;
private:
    std::string selector_, fields_, where_;
};
std::string serialize(const std::vector<Row>& rows, Format format);
}
#endif
