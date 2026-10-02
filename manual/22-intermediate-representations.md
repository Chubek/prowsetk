# Chapter 22: Intermediate Representations

[Manual index](README.md)

## Canonical page events

ProwseEvent is the canonical in-memory page IR. It records `start`, `attribute`,
`text`, and `end` events, with stable XPath-like paths, owning tags, values, and
depths. Start records additionally contain source-order attributes and subtree
text for compatibility projections. This stream describes document structure;
it does not require a rendering engine.

```cpp
const auto events = prowsetk::emit_prowse_events(*session->document());
const auto ndjson = prowsetk::encode_prowse_events_ndjson(events);
```

ProwseXAS is a compatibility spelling over the same records. ProwseDOM projects
start records into flattened nodes. New emitters and consumers should use the
canonical event stream rather than inventing a second DOM IR.

## Wire formats

| Name | Encoding | Typical consumer |
|---|---|---|
| `events` | One JSON event per line, NDJSON | Stream processors, page2pdf, page2latex |
| `vtd` | Binary ProwseVTD with `PVTD1` magic | page2pdf, bounded token decoder |
| `iml` | Nested ProwseIML S-expressions | page2pdf, macro-aware processors |

An NDJSON text record has fields such as:

```json
{"kind":"text","xpath":"/h1[1]/text()[1]","tag":"h1","name":"","value":"Report","depth":1}
```

Start records also carry `text` and an `attributes` array. Attribute records
have the owner tag in `tag`, attribute name in `name`, and attribute value in
`value`. Stream consumers use attribute/text events in order and avoid printing
start-record subtree text again.

VTD is little-endian: five-byte magic, 32-bit event count, then tokens with
type/depth and length-prefixed path/name/value fields. Type codes 1–4 correspond
to start/attribute/text/end. Attribute owners are reconstructed from the final
layout-path segment. `decode_prowse_vtd` rejects invalid/truncated streams with
an empty result and bounds reservation by available bytes.

IML represents elements and text inside a document form:

```lisp
(document
  (element h1 (@ (class "title"))
    (text "Report")
  )
)
```

The C++ `expand_prowse_iml` callback expands `(macro NAME "arg"...)` forms.
Returning an empty expansion preserves the macro. Expand macros before handing
IML to page2pdf.

## Lua emission and traversal

```lua
local ir = require('lprowseir')
local events = ir.emit_events(session)
local vtd_bytes = ir.emit(session, 'vtd')
local iml = ir.emit(session, 'iml')
local ndjson = ir.emit(session, 'events')
```

`emit_events` returns a Lua event array, while named `emit` returns text or
binary bytes as a Lua string. `emitters()` lists registered names. Compatibility
methods are `emit_xas`, `emit_dom`, `emit_vtd`, and `emit_iml`.

```lua
local count = ir.dom.walk(session, '//h1', function(element)
    assert(element:tag_name() == 'h1')
end)
local delivered = ir.xas:AddListener(session, '//h1', function(event)
    -- Consume the selected subtree's canonical records.
end)
```

`dom.walk` invokes the callback on matching managed elements. `xas:AddListener`
invokes it for selected stream records and returns a count. `xax` and
`add_listener` are aliases. Despite the listener name, the current call replays
the supplied document immediately; it is not a persistent future-DOM
subscription. Both validate XPath first and propagate callback errors.

## CLI and resource snapshots

```sh
prowsetk serialize --events --stdout --no-javascript --html '<h1>Report</h1>'
prowsetk serialize --iml --output build/page.iml --url https://example.com/
prowsetk serialize --vtd --output build/page.vtd --url https://example.com/
```

Live serialization can fetch bounded same-origin CSS and PNG/JPEG resources
through Session and embed them into the serialized document. Offline HTML makes
no resource requests and can carry inline CSS/image data URLs. IR serialization
preserves page content for downstream consumers; it is not the default-redacted
PDQL export path. Choose the input/output policy appropriate to the page.

## Custom emitters and implementation layers

IrEmitterRegistry::global() registers unique text/binary names. `register_text`
and `register_binary` reject empty names/callbacks; registering an existing name
replaces it. Emit operations return optional values for unknown names. Built-ins
are `events`, `iml`, and `vtd`; Lua resolves them through the same registry.

Keep semantics, layout/path assignment, XPath legality/filtering, format
lowering, and byte/text encoding separate. Downstream C consumers replay the
stream into their own sinks and do not access Flatworm C++ objects. Custom
emitters should consume the canonical semantics and retain stable paths.

Reference: `ir.hpp`, `src/core/ir_*.cpp`, `lua/lprowseir/init.lua`.

**Next:** [Page conversion tools](23-page-conversion-tools.md).
