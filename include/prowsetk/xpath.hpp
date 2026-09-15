#ifndef PROWSETK_XPATH_HPP
#define PROWSETK_XPATH_HPP

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "prowsetk/document.hpp"

namespace prowsetk {

// Result classification of an XPath 1.0 evaluation.
enum class XPathValueType {
    NodeSet,
    String,
    Number,
    Boolean,
};

// The result of evaluating an XPath 1.0 expression. A node-set carries the
// matched elements plus a parallel vector of string values; attribute and text
// results are mapped to their owning element while preserving the matched
// string value. Scalar results are reported through the matching field.
struct XPathValue {
    XPathValueType type = XPathValueType::NodeSet;

    // Node-set results. `nodes[i]` is the matched element (for element, text,
    // and attribute results); `string_values[i]` is the node's string value
    // (text content for elements, attribute value for attributes).
    std::vector<std::shared_ptr<Element>> nodes;
    std::vector<std::string> string_values;

    std::string string_value;
    double number_value = 0.0;
    bool boolean_value = false;
};

// Evaluates an XPath 1.0 expression against a document. Relative expressions
// are resolved against the document root. Throws Error(ParseError) for invalid
// expressions and Error(Unsupported) when ProwseTk is built without pugixml.
XPathValue evaluate_xpath(const Document& document,
                          std::string_view expression);

// Evaluates an XPath 1.0 expression against a single element subtree.
XPathValue evaluate_xpath(const Element& element,
                          std::string_view expression);

// Convenience: returns the string value of the first node-set match, or the
// scalar string value of the expression.
std::string xpath_string_value(const Document& document,
                               std::string_view expression);

}  // namespace prowsetk

#endif  // PROWSETK_XPATH_HPP