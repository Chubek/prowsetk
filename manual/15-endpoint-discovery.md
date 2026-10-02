# Chapter 15: Endpoint Discovery

[Manual index](README.md)

## Extracting from a document or session

The built-in EndpointExtractor finds candidate API operations from DOM links,
form actions/methods, inline script URL/call patterns, and supplied observations.
Discovery is heuristic; retain source, discovery method, confidence, and notes.

```lua
local endpoints = require('lprowsext.endpoints')
session:load_html([[
  <a href="/api/items?page=2">Items</a>
  <form action="/api/items" method="post"><input name="title"></form>
]], 'https://example.test/')
local result = endpoints.extract(session, { minimum_confidence = 0.40 })
assert(result:endpoint_count() == 2)
local yaml = result:openapi_yaml()
```

The Lua helper requires an already loaded document/session; a `url` field in
its options does not navigate. Use Session::navigate or the high-level
scrape-endpoints API to load a source first.

| Input | Additional discovery available |
|---|---|
| Bare Document | DOM and inline-script inspection |
| Lua Session extraction | Page-script requests and fetched external-script bodies |
| C++ EndpointExtractor | Explicit `observe` and `observe_script` inputs |
| Web interface | Host responses recorded by that interface's workflow |

External scripts and computed runtime URLs are visible only when the relevant
host has fetched/observed them. A bare document does not independently execute
scripts or request linked resources.

## Options

| EndpointExtractionOptions field | Default | Meaning |
|---|---|---|
| `follow_links` | true | Inspect link references in the supplied document |
| `inspect_scripts` | true | Inspect inline and supplied external script text |
| `observe_network` | true | Include supplied/request observations |
| `infer_schemas` | true | Include basic inferred OpenAPI structures |
| `include_provenance` | true | Preserve source/confidence metadata |
| `redact_secrets` | true | Redact secret-bearing output |
| `minimum_confidence` | 0.50 | Discovery threshold |
| `scrape_all_paths` | false | Admit ordinary links/resources beyond API markers |
| `openapi_version` | 3.1.0 | OpenAPI document version |
| `max_depth`, `max_pages` | 2, 100 | Options carried for surrounding workflows |

The core extraction call does not schedule a recursive site crawl. Use crawler,
spider, or explicit driver traversal for page visits. Lua's native option reader
accepts the core extraction fields; it is distinct from scrape-endpoints'
larger option table.

API-like anchor links have confidence 0.40 in the default discovery mode, below
the default 0.50 threshold. The example lowers the threshold explicitly.
`scrape_all_paths = true` broadens discovery and uses the comprehensive-mode
link confidences (0.75 for API-like links, 0.60 for ordinary links).

## Results and observations

Lua results expose methods `endpoints()`, `endpoint_count()`, `warnings()`,
`openapi_yaml()`, and `write_openapi_yaml(path)`. Each endpoint includes URL,
path, method, source, discovery method, confidence, parameter names, content
types, and notes. The file writer expects a usable parent directory; check its
success/error result.

```cpp
#include <fstream>

prowsetk::EndpointExtractor extractor;
extractor.observe("POST", "https://example.test/api/items", 201,
                  "application/json");
const auto result = extractor.extract(*session->document());
std::ofstream output("build/openapi.yaml");
output << result.openapi_yaml;
```

Session page observations include fetch/XHR/sendBeacon and supported dynamic
script/image loads. Script-initiated navigation preserves pre-navigation
observations; ordinary host navigation starts a new page observation lifecycle.
This is not a complete network HAR.

## Interpreting generated specifications

URLs resolve against the document base. Operations deduplicate by normalized
method/path, merging parameter names and preferring higher-confidence
representative provenance. The output uses OpenAPI-compatible method names,
inferred parameter/schema markers, and redacted URLs. Observed responses improve
provenance but do not establish all legal request bodies or response variants.

Use scrape-endpoints for shared OpenAPI/Postman filtering and resolution,
restful-resolver for bounded GET+POST discovery, and schema-grabber for typed
parameters and richer request/response inference. Their completeness/budget
metadata describes the performed work, not authoritative API coverage.

Reference: `endpoint_extraction.hpp`, `src/endpoint/endpoint_extraction.cpp`.

**Next:** [scrape-endpoints](16-scrape-endpoints.md).
