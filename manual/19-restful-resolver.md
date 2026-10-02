# Chapter 19: restful-resolver

[Manual index](README.md)

## Resolution model

restful-resolver expands a discovered endpoint set with bounded host-mediated
GET requests. It inspects resolved JSON for API-like references and reparses
resolved HTML for links, forms, and script calls. It stops when both a GET and
a POST endpoint are known, or when its budgets are exhausted.

`is_complete` has that specific GET+POST meaning. It does not establish that
every operation has been found or that the target implements a complete REST
design. All discoveries remain heuristic and carry provenance/confidence.

## Lua use

```lua
local resolver = dofile('plugins/restful-resolver/lua/restful_resolver.lua')
local result = resolver.resolve(session, {
    html = [[<a href="/api/items">Items</a>
              <form method="post" action="/api/items"></form>]],
    base_url = 'https://example.test/',
    minimum_confidence = 0.40,
    max_rounds = 0,
    max_requests = 0
})
assert(result.has_get and result.has_post and result.is_complete)
```

Set `url` for live loading, or omit source fields to reuse the current page.
An optional `endpoints` array supplies plain DiscoveredEndpoint-style seed
tables. Source loading and resolution are separate: an HTML override does not
by itself promise zero resolution requests. Use zero budgets/document-only
APIs when a hermetic run is required.

| Lua option | Default | Purpose |
|---|---|---|
| `max_rounds` | 4 | Maximum reference depth; seed requests are round 0 |
| `max_requests` | 64 | Fetch budget |
| `follow_json_links` | true | Discover referenced API URLs |
| `allow_cross_origin` / `allow_cross_origin_resolve` | false | Permit references outside current origin |
| `require_api_pattern` | true | API-like GET filtering; explicit non-GET operations retained |
| `api_patterns` | built-in API/admin markers | Candidate classification |
| `minimum_confidence` | 0.45 | Discovery threshold |
| `redact_secrets`, `include_provenance`, `infer_schemas` | true | Output policy |
| `openapi_version` | 3.1.0 | YAML version |

Requests run through Session::request. Browser transport policy still decides
which redirect hops and page/network origins are allowed. Apply a guarded host
transport when origin restrictions must cover every actual request.

## Methods, budgets, and result metadata

Resolution probes with GET. A POST operation discovered in a form/script or
seed set counts without sending a POST to validate it. A POST for an already
fetched URL can satisfy completeness without refetching the same URL.

Lua result fields include `endpoints`, `resolved`, `has_get`, `has_post`,
`is_complete`, `rounds_used`, `request_count`, and `warnings`.
Resolved entries preserve endpoint provenance, status/content type, final URL,
redirect information, and resolution round. Seed-only entries have no fetch
status. A seed set already containing GET+POST can complete before any probe.

The generic Lua Session response currently exposes status/body/final URL/headers
without a redirect-chain field, so normal Lua resolution records omit that
chain. The C++ session/resolver APIs retain it.

The C++ result additionally contains `openapi_yaml`, whose OpenAPI includes:

```yaml
x-prowsetk-restful:
  has-get: true
  has-post: true
  is-complete: true
  rounds-used: 0
  request-count: 0
```

This example describes seed-only completion, not site-wide coverage.

The Lua helper returns resolution data for a surrounding exporter. Pass
`result.endpoints` into schema-grabber to generate OpenAPI/Postman, or use
scrape-endpoints' render helpers. A driver that wants `x-prowsetk-restful`
metadata must add the Lua resolution summary explicitly.

## C++ and native plugin

The build-tree static helper exposes the
`prowsetk::plugins::restful_resolver` namespace. Its public functions are
`resolve_from_document`, `resolve_from_session`, and the high-level `resolve`.
The document variant never touches the network; the session variant probes
through the owning session. RestfulResolverOptions and RestfulResolverResult
define the typed contract.

The shared native plugin advertises capabilities and initialization/shutdown
metadata. Its current entrypoint does not implement automatic request/document
resolution hooks. Loading `libprowsetk_restful_resolver.so` is therefore separate
from calling the Lua/C++ resolver.

## Composition

Use scrape-endpoints first, pass its plain filtered seeds to resolution, then
pass the resolved endpoint set to schema-grabber for parameter/body enrichment.
The Booking example follows this ordering and adds explicit incomplete coverage
metadata to the final export. Configure each stage's budget: request counts in
one stage are not an unlimited quota for the others.

Reference: [resolver notes](../plugins/restful-resolver/README.md),
`plugins/restful-resolver/include/prowsetk/plugins/restful_resolver.hpp`.

**Next:** [schema-grabber](20-schema-grabber.md).
