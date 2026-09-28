// ir_model.cpp — semantics stratum.
//
// Sole owner of meaning: lowers the Flatworm DOM to the canonical model
// (ProwseEvent start/attribute/text/end events) in a single deterministic
// pre-order walk. Start events carry the flattened-node snapshot so a second
// DOM-shaped IR is not needed. Knows nothing about XPath, binary
// framing, or text escaping; those belong to the legality and encoding
// strata, which consume the canonical model read-only.

#include "ir_internal.hpp"

namespace prowsetk {
namespace ir {
namespace {

ProwseEvent make_event(std::string kind, std::string xpath, std::string tag,
                       std::string name, std::string value,
                       std::size_t depth) {
    ProwseEvent event;
    event.kind = std::move(kind);
    event.xpath = std::move(xpath);
    event.tag = std::move(tag);
    event.name = std::move(name);
    event.value = std::move(value);
    event.depth = depth;
    return event;
}

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

    ProwseEvent start = make_event("start", path, node->name, {}, {}, depth);
    start.subtree_text = flatworm::text_content(*node);
    start.attributes.reserve(node->attributes.size());
    for (const auto& attr : node->attributes) {
        start.attributes.push_back(Attribute{attr.name, attr.value});
    }
    out.events.push_back(std::move(start));
    for (const auto& attr : node->attributes) {
        out.events.push_back(make_event("attribute", path, node->name,
                                        attr.name, attr.value, depth));
    }

    std::size_t text_index = 0;
    for (const auto& child : node->children) {
        if (child->type == flatworm::NodeType::Text &&
            is_renderable_text(child->text)) {
            ++text_index;
            out.events.push_back(make_event(
                "text", path + "/text()[" + std::to_string(text_index) + "]",
                node->name, {}, child->text, depth + 1));
        }
        if (child->type == flatworm::NodeType::Element) {
            walk_tree(child, depth + 1, out);
        }
    }

    out.events.push_back(make_event("end", path, node->name, {}, {}, depth));
}

}  // namespace

IrBuild build_ir(const Document& document) {
    IrBuild build;
    const auto& root = document.raw_root();
    walk_tree(root, 0, build);
    return build;
}

}  // namespace ir

ProwseEventStream emit_prowse_events(const Document& document) {
    return ir::build_ir(document).events;
}

ProwseEventStream emit_prowse_xas(const Document& document) {
    return emit_prowse_events(document);
}

std::vector<ProwseDomNode> emit_prowse_dom(const Document& document) {
    const auto events = emit_prowse_events(document);
    std::vector<ProwseDomNode> nodes;
    nodes.reserve(events.size() / 2);
    for (const auto& event : events) {
        if (event.kind == "start") {
            nodes.push_back(ProwseDomNode{event.xpath, event.tag,
                                           event.subtree_text,
                                           event.attributes, event.depth});
        }
    }
    return nodes;
}

}  // namespace prowsetk
