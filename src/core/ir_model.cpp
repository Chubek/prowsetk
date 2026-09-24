// ir_model.cpp — semantics stratum.
//
// Sole owner of meaning: lowers the Flatworm DOM to the canonical model
// (ProwseDomNode records plus ProwseXAS start/attribute/text/end events) in a
// single deterministic pre-order walk. Knows nothing about XPath, binary
// framing, or text escaping; those belong to the legality and encoding
// strata, which consume the canonical model read-only.

#include "ir_internal.hpp"

namespace prowsetk {
namespace ir {
namespace {

void walk_tree(const std::shared_ptr<flatworm::Node>& node, std::size_t depth,
               IrBuild& out) {
    if (node == nullptr) {
        return;
    }
    if (node->type == flatworm::NodeType::Document) {
        for (const auto& child : node->children) {
            walk_tree(child, depth, out);
        }
        return;
    }
    if (node->type != flatworm::NodeType::Element) {
        return;
    }

    const std::string path = element_path(node);
    out.node_paths[node.get()] = path;

    ProwseDomNode dom;
    dom.xpath = path;
    dom.tag = node->name;
    dom.text = flatworm::text_content(*node);
    dom.depth = depth;
    dom.attributes.reserve(node->attributes.size());
    for (const auto& attr : node->attributes) {
        dom.attributes.push_back(Attribute{attr.name, attr.value});
    }
    out.dom_nodes.push_back(std::move(dom));

    out.xas_events.push_back(
        ProwseXasEvent{"start", path, node->name, {}, {}, depth});
    for (const auto& attr : node->attributes) {
        out.xas_events.push_back(ProwseXasEvent{"attribute", path, node->name,
                                                attr.name, attr.value, depth});
    }

    std::size_t text_index = 0;
    for (const auto& child : node->children) {
        if (child->type == flatworm::NodeType::Text &&
            is_renderable_text(child->text)) {
            ++text_index;
            out.xas_events.push_back(ProwseXasEvent{
                "text", path + "/text()[" + std::to_string(text_index) + "]",
                node->name, {}, child->text, depth + 1});
        }
        if (child->type == flatworm::NodeType::Element) {
            walk_tree(child, depth + 1, out);
        }
    }

    out.xas_events.push_back(
        ProwseXasEvent{"end", path, node->name, {}, {}, depth});
}

}  // namespace

IrBuild build_ir(const Document& document) {
    IrBuild build;
    const auto& root = document.raw_root();
    walk_tree(root, 0, build);
    return build;
}

}  // namespace ir

std::vector<ProwseXasEvent> emit_prowse_xas(const Document& document) {
    return ir::build_ir(document).xas_events;
}

std::vector<ProwseDomNode> emit_prowse_dom(const Document& document) {
    return ir::build_ir(document).dom_nodes;
}

}  // namespace prowsetk
