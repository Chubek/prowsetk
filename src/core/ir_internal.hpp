// ir_internal.hpp — shared contracts between the IR strata.
//
// This header is internal to `src/core` (never installed). Each stratum owns
// the declarations in its section; other strata consume them read-only:
//
// - ir_layout.cpp owns stable path/depth assignment.
// - ir_model.cpp owns the canonical DOM walk.
// - ir_filter.cpp owns XPath selection.
// - ir_vtd.cpp / ir_iml.cpp own serialization of the canonical model.
//
// Cross-stratum short-circuits (for example, an encoder re-walking the DOM or
// a filter reimplementing path layout) are layering violations.

#ifndef PROWSETK_IR_INTERNAL_HPP
#define PROWSETK_IR_INTERNAL_HPP

#include <cstddef>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "flatworm/dom_internal.hpp"
#include "prowsetk/document.hpp"
#include "prowsetk/ir.hpp"

namespace prowsetk::ir {

// --- Register/layout stratum (ir_layout.cpp) --------------------------------

// True for text that survives into IRs; whitespace-only nodes are dropped.
bool is_renderable_text(const std::string& text);

// Stable XPath-like path for an element node, e.g. "/html[1]/body[1]/a[2]".
// Sibling indexes count same-tag elder siblings only. Returns empty for
// null or non-element nodes.
std::string element_path(const std::shared_ptr<flatworm::Node>& node);

// Recovers the owning element tag from an XPath-like element path by stripping
// the trailing "[n]" index from the final segment. Returns empty when the
// path has no element segment.
std::string tag_from_path(std::string_view path);

// True when `candidate` equals `scope` or names a strict descendant of it
// (`scope + "/..."`). Used by the legality stratum to test event membership
// without reimplementing layout.
bool is_within_scope(std::string_view candidate, std::string_view scope);

// --- Semantics stratum (ir_model.cpp) ---------------------------------------

struct IrBuild {
    std::vector<ProwseDomNode> dom_nodes;
    std::vector<ProwseXasEvent> xas_events;
    std::unordered_map<const flatworm::Node*, std::string> node_paths;
};

// Lowers the whole document to the canonical model in one deterministic walk.
// An invalid document (no DOM tree) yields an empty build.
IrBuild build_ir(const Document& document);

}  // namespace prowsetk::ir

#endif  // PROWSETK_IR_INTERNAL_HPP
