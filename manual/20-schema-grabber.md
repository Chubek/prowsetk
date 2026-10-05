# Chapter 20: schema-grabber

[Manual index](README.md)

## Discovery followed by enrichment

schema-grabber adds typed query/path parameters, inferred request schemas, and
response schemas to discovered endpoints. It emits deterministic OpenAPI 3.x
YAML and Postman 2.1 JSON with provenance and inference markers.

```lua
local scrape = dofile('plugins/scrape-endpoints/lua/scrape_endpoints.lua')
local grabber = dofile('plugins/schema-grabber/lua/schema_grabber.lua')
local discovered = scrape.scrape(session, {
    html = '<a href="/api/items/123?active=true">Item</a>',
    base_url = 'https://example.test/',
    minimum_confidence = 0.40,
    spa_probe = false,
    resolve_chain = false
})
local enriched = grabber.enrich(session, {
    endpoints = discovered.filtered_endpoints,
    probe_get_responses = false
})
assert(enriched.schema_count == 1 and enriched.probe_count == 0)
```

`enrich` and its alias `grab` accept a managed Session or Document. A seed array
is optional; without it the helper extracts endpoints. `url`, `html`, and
`base_url` support source loading for session use. Explicitly disable GET probes
for deterministic offline enrichment.

## Inference rules

The C++ implementation applies the following evidence hierarchy:

| Input | Inferred output |
|---|---|
| Query examples | boolean/integer/number/string; `required: false` |
| Numeric IDs, UUIDs, long hashes in paths | `{id}`-style templates with example values |
| Matching POST/PUT/PATCH form controls | Typed request fields, HTML `required` flags |
| JSON body hints near endpoint paths in inline scripts | Inferred request objects |
| Known request content type without detailed fields | Generic inferred body object |
| Observed JSON response bytes | Bounded typed object/array/property schema |

GET operations have no inferred request body. A URL example cannot establish
query-parameter requirement. A single observed response does not establish all
legal variants, nullability, or required properties across a real API.

Request inference prefers form evidence, then nearby script body hints, then
generic content-type evidence. Response inference uses available bytes; generic
inferred responses remain marked as such when bytes are unavailable.

The current Lua helper implements typed URL parameters, matching form request
fields, and GET-response JSON inference. Script-body literals, generic
request-body fallback, and reuse of externally supplied `ResolvedBody` records
are available through the C++ API. Lua can also infer matched DELETE form
fields. Keep these implementation differences in mind when choosing the
enrichment path.

Lua records a matched form field's `required` flag in its returned schema
records, but the current Lua OpenAPI renderer does not emit the request
object's corresponding `required` array. Postman request examples also omit
that validation constraint. For assistant snapshot composition and its export
policy, see [Chapter 32](32-lua-snapshots-and-api-discovery.md).

## Probes and limits

| Option | Default | Meaning |
|---|---|---|
| `probe_get_responses` | true | Probe GET endpoints when a Session is available |
| `max_probe_requests` | 32 | GET probe budget |
| `allow_cross_origin` / `allow_cross_origin_resolve` | false | Probe origin policy |
| `max_body_bytes` | 262144 | C++ body inference bound; oversized bodies marked truncated |
| `max_properties`, `max_depth` | 64, 4 | Schema inference budgets |
| `minimum_confidence` | 0.50 | Discovery threshold |
| `require_api_pattern` | true | API-like filtering with explicit non-GET retention |
| `redact_secrets`, `include_provenance`, `include_examples`, `infer_schemas` | true | Output policy |
| `output`, `output_postman` | empty | Optional output files |
| `collection_name` | Discovered API (schema-grabber) | Collection title |

Only GET endpoints are probed, using host-mediated GET. POST/PUT/PATCH are
never sent with their own method merely to infer a response. Already observed
body bytes can enrich those methods without a new side-effecting request.

Lua normalizes `max_body_bytes` but currently does not apply that truncation
before JSON decoding. Its depth/property options restrict parts of inference;
the C++ implementation enforces the documented body/schema budgets. In Lua
workflows, set the owning Browser/transport response-size bound explicitly.

Origin checks at the enrichment layer select probes; the host transport
controls redirect hops and other actual network operations. Crawler and spider
supply guarded transports; generic embedders can install their own.

## Results and output

Lua results contain `schemas`, `endpoints`, `warnings`, `probe_count`,
`schema_count`, `openapi_yaml`, and `postman_json`, plus file-writing methods.
OpenAPI includes `x-prowsetk-schema` request/response provenance along with
confidence, normal provenance, and `x-inferred`. Postman includes typed query
pairs, `:var` path variables, example request bodies, and available example
responses.

Sensitive field names and form/JSON/query values retain type information while
their examples are redacted by default. Disable examples if an application
does not need them; inference remains heuristic.

## C++ composition and native hook

The C++ API exposes `grab_from_document`, `grab_from_document_with_bodies`,
`grab_from_session`, and `grab`. `enrich_endpoints` accepts plain endpoints and
`ResolvedBody` records with method/path, status, content type, and observed body.
That value-only record avoids a link dependency between the scraper and
grabber plugins and permits reuse of already fetched responses.

The native shared plugin's configured document hook performs snapshot-only
enrichment and writes selected outputs. It cannot probe through a live Session;
use the explicit C++/Lua session API for probes. Its native configuration
accepts `schema-grabber.output_openapi`, `schema-grabber.output_postman`, and
the selected options listed in `src/plugin_entry.cpp`.

`wit/schema-grabber.wit` specifies a future WASM contract; the current usable
implementations are native C++ helpers and Lua, with the general WASM runtime
still reporting disabled support.

Reference: [schema-grabber notes](../plugins/schema-grabber/README.md),
`plugins/schema-grabber/include/prowsetk/plugins/schema_grabber.hpp`.

**Next:** [Native and WASM plugins](21-native-and-wasm-plugins.md).
