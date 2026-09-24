#ifndef PROWSETK_IR_HPP
#define PROWSETK_IR_HPP

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "prowsetk/document.hpp"

namespace prowsetk {

// Intermediate representations for a loaded page (AGENTS.md "The IR Emitters").
//
// The implementation is stratified into five concerns with hard boundaries
// (AGENTS.md section 7). Each stratum lives in its own translation unit under
// `src/core/` and communicates only through the narrow contracts noted here:
//
// - **Semantics** (`ir_model.cpp`): walks the Flatworm DOM and lowers it to a
//   canonical event/node model. Defines what the IRs *mean*; knows nothing
//   about XPath, binary layouts, or text escaping.
// - **Register/layout** (`ir_layout.cpp`): assigns stable XPath-like paths,
//   sibling indexes, and depths. The only place that computes layout; every
//   emitter consumes its assignments verbatim.
// - **Legality** (`ir_filter.cpp`, `xpath.hpp`): validates XPath expressions
//   and selects subtrees. The only place that interprets XPath; never emits.
// - **Lowering** (per-emitter, inside each `ir_*.cpp`): maps the canonical
//   model to one IR shape (event stream, flattened nodes, binary tokens,
//   S-expressions). Never reparses HTML and never touches the DOM directly.
// - **Encoding** (`ir_vtd.cpp`, `ir_iml.cpp`): serializes lowered IRs to
//   bytes or text (VTD framing, IML escaping, macro expansion). Never walks
//   the DOM.
//
// Plugin extensibility: additional IRs register named text/binary emitters
// with IrEmitterRegistry instead of patching the built-ins. The Lua
// `lprowseir` pipeline (`emit`, `emitters`) resolves through the same
// registry, so C++ plugins and Lua drivers observe identical IR names.

// ProwseXAS event emitted while walking a document tree.
struct ProwseXasEvent {
    // "start", "attribute", "text", or "end".
    std::string kind;
    // XPath-like stable path (for example: /html[1]/body[1]/a[2]).
    std::string xpath;
    // Element tag name for element-scoped events. For "attribute" events this
    // names the owning element; `name` carries the attribute name.
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

// ProwseVTD binary framing (encoding stratum). The stream is little-endian:
// magic "PVTD1", u32 event count, then per-event tokens of
// (u8 type, u32 depth, len-prefixed path, name, value). Type codes are
// 1=start, 2=attribute, 3=text, 4=end. For attribute tokens `name` carries
// the attribute name; the owning element tag is recoverable from `xpath`
// (decoders repopulate `tag` from the path's final segment).
inline constexpr char kProwseVtdMagic[5] = {'P', 'V', 'T', 'D', '1'};
inline constexpr std::uint8_t kProwseVtdStart = 1;
inline constexpr std::uint8_t kProwseVtdAttribute = 2;
inline constexpr std::uint8_t kProwseVtdText = 3;
inline constexpr std::uint8_t kProwseVtdEnd = 4;

// Emits a compact binary token stream.
std::vector<std::uint8_t> emit_prowse_vtd(const Document& document);

// Decodes a ProwseVTD token stream back into ProwseXAS-style events. Invalid
// or truncated input returns an empty vector. Decoding is allocation-bounded:
// reservation is capped by the input size so a corrupt event count cannot
// force a huge allocation.
std::vector<ProwseXasEvent> decode_prowse_vtd(std::span<const std::uint8_t> bytes);

// Emits an S-expression textual form. Returns an empty string when the
// document has no DOM tree; otherwise emits a `(document ...)` form which may
// carry no element children.
std::string emit_prowse_iml(const Document& document);

using ProwseImlMacroExpander =
    std::function<std::string(std::string_view name,
                              const std::vector<std::string>& arguments)>;

// Expands simple ProwseIML macro forms: (macro <name> "arg"...). Unknown
// macros are preserved. String arguments support the same escapes emitted by
// emit_prowse_iml.
std::string expand_prowse_iml(std::string_view iml,
                              const ProwseImlMacroExpander& expander);

// --- Legality stratum --------------------------------------------------------

// Outcome of validating an XPath expression without evaluating it.
struct XPathLegality {
    bool ok = false;
    // Non-empty only when `ok` is false.
    std::string error;
};

// Validates `expression` for use by walkers and listeners. Empty expressions
// are illegal; without pugixml every non-empty expression is reported illegal
// with an "unsupported" diagnostic rather than throwing.
XPathLegality check_xpath_legality(const Document& document,
                                   std::string_view expression);

// Non-throwing convenience over check_xpath_legality.
bool is_valid_xpath_expression(const Document& document,
                               std::string_view expression);

// --- Plugin-extensible named emitters ----------------------------------------

using IrTextEmitter = std::function<std::string(const Document&)>;
using IrBinaryEmitter = std::function<std::vector<std::uint8_t>(const Document&)>;

// Registry for named IR emitters. The built-ins register "iml" (text) and
// "vtd" (binary) at startup; native plugins add further formats without
// touching the built-in emitters or the C plugin ABI. Names are unique:
// registering an existing name replaces the previous emitter. All methods
// are thread-safe.
class IrEmitterRegistry {
public:
    static IrEmitterRegistry& global();

    // Returns false when `name` is empty or the emitter is empty.
    bool register_text(std::string_view name, IrTextEmitter emitter);
    bool register_binary(std::string_view name, IrBinaryEmitter emitter);
    bool unregister(std::string_view name);
    void clear_custom();

    bool contains(std::string_view name) const;
    std::vector<std::string> names() const;

    // Emits through the named emitter. Returns nullopt for unknown names.
    std::optional<std::string> emit_text(const Document& document,
                                         std::string_view name) const;
    std::optional<std::vector<std::uint8_t>> emit_binary(
        const Document& document, std::string_view name) const;

private:
    IrEmitterRegistry();
    mutable std::mutex mutex_;
    std::unordered_map<std::string, IrTextEmitter> text_;
    std::unordered_map<std::string, IrBinaryEmitter> binary_;
};

}  // namespace prowsetk

#endif  // PROWSETK_IR_HPP
