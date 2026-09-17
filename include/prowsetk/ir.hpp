#ifndef PROWSETK_IR_HPP
#define PROWSETK_IR_HPP

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "prowsetk/document.hpp"

namespace prowsetk {

// ProwseXAS event emitted while walking a document tree.
struct ProwseXasEvent {
    // "start", "attribute", "text", or "end".
    std::string kind;
    // XPath-like stable path (for example: /html[1]/body[1]/a[2]).
    std::string xpath;
    // Element tag name for element-scoped events.
    std::string tag;
    // Attribute name for "attribute" events.
    std::string name;
    // Text value (for text and attribute events).
    std::string value;
    std::size_t depth = 0;
};

// Flattened DOM node record, suitable for visitor/walker implementations.
struct ProwseDomNode {
    std::string xpath;
    std::string tag;
    std::string text;
    std::vector<Attribute> attributes;
    std::size_t depth = 0;
};

// Emits a structured event stream for the current document.
std::vector<ProwseXasEvent> emit_prowse_xas(const Document& document);

// Filters the event stream using an XPath expression against the underlying DOM.
// If XPath support is unavailable in this build, this returns an empty list.
std::vector<ProwseXasEvent> filter_prowse_xas(const Document& document,
                                              std::string_view xpath_expression);

// Emits a flattened DOM representation with stable paths.
std::vector<ProwseDomNode> emit_prowse_dom(const Document& document);

// Emits a compact binary token stream.
std::vector<std::uint8_t> emit_prowse_vtd(const Document& document);

// Decodes a ProwseVTD token stream back into ProwseXAS-style events. Invalid
// or truncated input returns an empty vector.
std::vector<ProwseXasEvent> decode_prowse_vtd(std::span<const std::uint8_t> bytes);

// Emits an S-expression textual form.
std::string emit_prowse_iml(const Document& document);

using ProwseImlMacroExpander =
    std::function<std::string(std::string_view name,
                              const std::vector<std::string>& arguments)>;

// Expands simple ProwseIML macro forms: (macro <name> "arg"...). Unknown
// macros are preserved. String arguments support the same escapes emitted by
// emit_prowse_iml.
std::string expand_prowse_iml(std::string_view iml,
                              const ProwseImlMacroExpander& expander);

}  // namespace prowsetk

#endif  // PROWSETK_IR_HPP
