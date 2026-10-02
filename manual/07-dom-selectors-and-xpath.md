# Chapter 07: DOM, Selectors, and XPath

[Manual index](README.md)

## Loading and inspecting a DOM

`parse_html` creates a Document directly. A Session installs one through
navigation or `load_html`. Document provides title, text, HTML, metadata, links,
forms, scripts, resource URLs, its root, and element creation. Element provides
attributes, subtree text/HTML, form values, parents, children, siblings,
selectors, and mutation.

```lua
local doc = session:document()
local heading = doc:query_selector('main > h1')
if heading then
    heading:set_text('Updated title')
    heading:set_attribute('data-processed', 'true')
end
for _, link in ipairs(doc:query_selector_all('a[href]')) do
    local resolved = require('lprowse').url.resolve(doc:url(), link:attribute('href'))
end
```

Direct getters return raw page data. Default-redacted PDQL snapshots and daemon
exports are separate APIs with explicit output policies.

## Built-in HTML parser

The tolerant Flatworm parser accepts malformed markup within a restricted HTML
tree-construction model. It keeps the first duplicate attribute, accepts
punctuation in attribute names, supports scoped omitted-end-tag recovery, and
handles delimited raw-text/RCDATA end tags. Non-void start tags ignore a trailing
solidus. Numeric references support invalid-scalar replacement and HTML C1
remapping; named references use the documented, semicolon-terminated vocabulary.

Each parse is bounded to 16 MiB input, 250,000 total nodes including the
document, and 256 levels below the document. Fragment parsing shares these
bounds. They are per-parse limits, rather than a cumulative mutation quota.

The parser does not insert implicit html/head/body or table wrappers, implement
foster parenting/adoption-agency reconstruction, provide foreign-content
namespaces, or implement full HTML5 fragment insertion modes. Parser backends
can be selected through `HtmlParser` and `parse_html_with`; query the backend's
availability before selecting an optional one.

`BrowserConfig::html_parser` defaults to `HtmlParser::Builtin`. `Auto` prefers
Lexbor, then Gumbo, then the built-in parser according to build availability.
The optional backends copy their parsed trees into Flatworm's shared DOM model.

## CSS selector reference

| Feature | Examples |
|---|---|
| Type, universal, class, ID | `h1`, `*`, `.card`, `#main` |
| Attributes and operators | `[href]`, `[type="email"]`, `[class~="card"]`, `[lang|="en"]`, `[href^="/"]`, `[href$=".json"]`, `[href*="api"]` |
| Attribute case flags | `[data-kind="BOOK" i]`, `[data-kind="BOOK" s]` |
| Combinators | `main p`, `main > p`, `h1 + p`, `h1 ~ p` |
| Lists | `h1, h2, h3` |
| Structural pseudo-classes | `:first-child`, `:last-child`, `:only-child`, `:nth-child(2n+1)` and reverse/of-type variants |
| Other supported pseudo-classes | `:empty`, `:root`, complex/list/nested `:not()` |

CSS escapes are decoded by the shared selector parser. Unflagged attribute
values remain case-sensitive. Unsupported syntax, including `:is`, `:where`,
`:has`, pseudo-elements, and namespaces, fails explicitly.

Queries preserve document order and deduplicate matches. Element queries
exclude the receiver itself; use `matches` to test it. Syntax is bounded to
64 KiB, 256 compounds, and 32 nested negations. Matching is bounded to 100,000
compound evaluations per candidate.

## XPath

Pugixml supplies XPath 1.0 over the DOM snapshot. In Lua:

```lua
local titles = session:document():xpath_strings('//main/h1/text()')
local count = session:document():xpath('count(//a[@href])')
assert(type(count) == 'number')
```

C++ uses `evaluate_xpath(document_or_element, expression)`. `XPathValue` can be
a node-set, string, number, or boolean. For node-sets, `nodes` holds element
wrappers and `string_values` holds the actual selected values. Text and
attribute matches map to their owning element; use string values when the
selected text/attribute is what you need.

Relative element expressions use that element context. Invalid XPath reports
ParseError; a build without pugixml reports Unsupported. PDQL and IR walkers
reuse this engine.

## Mutation and interaction

Create detached nodes with `create_element`, then attach with `append_child`.
`remove_child` detaches; `set_text` replaces children. Mutations emit DOM events
through the owning session. `set_value` changes a property; framework-aware
typing and clicks go through a managed session, described in Chapter 12.

Reference: `document.hpp`, `xpath.hpp`, `src/flatworm/`, selector unit tests.

**Next:** [PDQL](08-pdql.md).
