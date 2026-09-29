# PDQL implementation status

This archive adds an initial, lightweight PDQL implementation to ProwseTk.
It consists of the C++ API (`include/prowsetk/pdql.hpp`), a C-compatible API
(`include/prowsetk/pdql.h`), and their implementation (`src/pdql/pdql.cpp`).
The source file is registered in `src/CMakeLists.txt` as part of the core
library sources.

## Current behavior

- Parses a basic HTML-like string into a tree of tags, attributes, and text.
  Tag and attribute names are lowercased; comments and declarations are skipped;
  a small set of common void tags is treated as non-nesting.
- Supports a deliberately small query subset: selector-only queries or
  `select <fields> from <selector> [where <condition>]`; wildcard tag patterns,
  a basic `xpath(...)` form with a limited attribute predicate, and a simple
  `rx"..."` text regular-expression predicate are implemented.
- Provides fields for text, tag, attributes, count, numeric conversion, sum,
  and average, plus simple equality filtering on text/tag/attribute values.
- Serializes rows as JSON, YAML, XML, or S-expressions. The C API wraps document,
  query, and result lifetimes and provides an allocated-string free function.

## Scope and limitations

This is a compact initial implementation, not a full HTML5 parser, XPath/CSS
engine, or complete PDQL specification. Parsing is permissive and simplistic;
for example, HTML entities are not decoded, and the parser does not implement
browser error recovery or the full HTML grammar. Selector/query syntax and
`where` expressions are limited to the cases handled in `pdql.cpp`; aggregation
and serialization are intentionally basic and should not be assumed to cover
all edge cases or escaping requirements of production formats. Validate this
implementation against the project's intended PDQL contract before relying on
it for general-purpose scraping.

No build or tests are claimed as part of this packaging operation.
