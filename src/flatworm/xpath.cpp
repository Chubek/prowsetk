#include "prowsetk/xpath.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "flatworm/dom_internal.hpp"
#include "prowsetk/error.hpp"

#ifdef PROWSETK_HAVE_PUGIXML
#include <pugixml.hpp>
#endif

namespace prowsetk {
namespace {

namespace fw = flatworm;

#ifdef PROWSETK_HAVE_PUGIXML

using NodeMap =
    std::unordered_map<const pugi::xml_node_struct*, std::shared_ptr<fw::Node>>;

// Mirrors a flatworm subtree into the pugi::xml_document, recording the
// correspondence between every created node and its flatworm counterpart.
void mirror(const std::shared_ptr<fw::Node>& flat, pugi::xml_node& parent,
            NodeMap& nodes) {
    if (flat == nullptr) {
        return;
    }
    switch (flat->type) {
        case fw::NodeType::Document:
            for (const auto& child : flat->children) {
                mirror(child, parent, nodes);
            }
            return;
        case fw::NodeType::Element: {
            pugi::xml_node element = parent.append_child(pugi::node_element);
            element.set_name(flat->name.c_str());
            for (const auto& attr : flat->attributes) {
                pugi::xml_attribute attribute =
                    element.append_attribute(attr.name.c_str());
                attribute.set_value(attr.value.c_str());
            }
            nodes[element.internal_object()] = flat;
            for (const auto& child : flat->children) {
                mirror(child, element, nodes);
            }
            return;
        }
        case fw::NodeType::Text: {
            pugi::xml_node text = parent.append_child(pugi::node_pcdata);
            text.set_value(flat->text.c_str());
            nodes[text.internal_object()] = flat;
            return;
        }
        case fw::NodeType::Comment: {
            pugi::xml_node comment = parent.append_child(pugi::node_comment);
            comment.set_value(flat->text.c_str());
            nodes[comment.internal_object()] = flat;
            return;
        }
        case fw::NodeType::Doctype: {
            pugi::xml_node doctype = parent.append_child(pugi::node_doctype);
            doctype.set_name(flat->name.c_str());
            nodes[doctype.internal_object()] = flat;
            return;
        }
    }
}

std::string node_string_value(const pugi::xpath_node& result) {
    if (result.attribute()) {
        return result.attribute().value();
    }
    return result.node().text().get();
}

std::shared_ptr<Element> wrap(const std::shared_ptr<fw::Node>& node) {
    if (node == nullptr) {
        return nullptr;
    }
    return std::make_shared<Element>(node);
}

XPathValue evaluate_impl(const std::shared_ptr<fw::Node>& root,
                         std::string_view expression) {
    XPathValue value;
    if (expression.empty()) {
        throw Error(ErrorCode::ParseError, "XPath expression is empty");
    }

    pugi::xml_document document;
    NodeMap nodes;
    pugi::xml_node context = document;
    if (root->type != fw::NodeType::Document) {
        // Element-scoped evaluation: mirror the element itself as the context
        // node so relative paths resolve against the element's subtree.
        context = document.append_child(pugi::node_element);
        context.set_name(root->name.c_str());
        for (const auto& attr : root->attributes) {
            pugi::xml_attribute attribute =
                context.append_attribute(attr.name.c_str());
            attribute.set_value(attr.value.c_str());
        }
        nodes[context.internal_object()] = root;
        for (const auto& child : root->children) {
            mirror(child, context, nodes);
        }
    } else {
        mirror(root, context, nodes);
    }

    try {
        const std::string expression_text(expression);
        const pugi::xpath_query query(expression_text.c_str());
        switch (query.return_type()) {
            case pugi::xpath_type_string:
                value.type = XPathValueType::String;
                value.string_value = query.evaluate_string(context);
                return value;
            case pugi::xpath_type_number:
                value.type = XPathValueType::Number;
                value.number_value = query.evaluate_number(context);
                return value;
            case pugi::xpath_type_boolean:
                value.type = XPathValueType::Boolean;
                value.boolean_value = query.evaluate_boolean(context);
                return value;
            case pugi::xpath_type_node_set:
            default: {
                value.type = XPathValueType::NodeSet;
                const pugi::xpath_node_set set =
                    query.evaluate_node_set(context);
                value.nodes.reserve(set.size());
                value.string_values.reserve(set.size());
                for (const auto& result : set) {
                    // For attribute results pugixml's node() returns the owning
                    // element; the attribute value is preserved separately.
                    const auto found =
                        nodes.find(result.node().internal_object());
                    value.nodes.push_back(wrap(found != nodes.end()
                                                   ? found->second
                                                   : nullptr));
                    value.string_values.push_back(node_string_value(result));
                }
                return value;
            }
        }
    } catch (const pugi::xpath_exception& error) {
        throw Error(ErrorCode::ParseError,
                    std::string("invalid XPath expression: ") + error.what());
    }
}

#endif  // PROWSETK_HAVE_PUGIXML

}  // namespace

XPathValue evaluate_xpath(const Document& document,
                          std::string_view expression) {
#ifdef PROWSETK_HAVE_PUGIXML
    const auto& root = document.raw_root();
    if (root == nullptr) {
        throw Error(ErrorCode::Internal, "document has no DOM tree");
    }
    return evaluate_impl(root, expression);
#else
    (void)document;
    (void)expression;
    throw Error(ErrorCode::Unsupported,
                "XPath requires a ProwseTk build with pugixml");
#endif
}

XPathValue evaluate_xpath(const Element& element,
                          std::string_view expression) {
#ifdef PROWSETK_HAVE_PUGIXML
    const auto& node = element.node();
    if (node == nullptr) {
        throw Error(ErrorCode::Internal, "element has no backing node");
    }
    return evaluate_impl(node, expression);
#else
    (void)element;
    (void)expression;
    throw Error(ErrorCode::Unsupported,
                "XPath requires a ProwseTk build with pugixml");
#endif
}

std::string xpath_string_value(const Document& document,
                               std::string_view expression) {
    const XPathValue value = evaluate_xpath(document, expression);
    if (value.type == XPathValueType::String) {
        return value.string_value;
    }
    if (value.type == XPathValueType::NodeSet && !value.string_values.empty()) {
        return value.string_values.front();
    }
    if (value.type == XPathValueType::Number) {
        return std::to_string(value.number_value);
    }
    if (value.type == XPathValueType::Boolean) {
        return value.boolean_value ? "true" : "false";
    }
    return {};
}

}  // namespace prowsetk