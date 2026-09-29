#ifndef PROWSETK_PDQL_HPP
#define PROWSETK_PDQL_HPP
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
namespace prowsetk::pdql {
enum class Format { Json, Yaml, Xml, SExpr };
struct Node { std::string tag, text; std::map<std::string,std::string> attributes; std::vector<std::unique_ptr<Node>> children; Node *parent=nullptr; };
class Document { public: static Document parse(std::string_view html); const Node& root() const { return *root_; } private: std::unique_ptr<Node> root_; friend class Query; };
struct Value { std::string text; bool numeric=false; };
struct Row { std::map<std::string,Value> fields; };
class Query { public: explicit Query(std::string_view source); std::vector<Row> execute(const Document& document) const; private: std::string selector_, fields_, where_; };
std::string serialize(const std::vector<Row>& rows, Format format);
}
#endif
