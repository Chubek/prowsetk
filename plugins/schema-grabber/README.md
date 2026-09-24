# schema-grabber

Native ProwseTk plugin that reverse-engineers scraped endpoints into full
request/response schemas plus URL parameters. It composes with
`plugins/scrape-endpoints`: scrape first for discovery, then enrich the same
endpoints so the exported OpenAPI and Postman specs carry typed query/path
parameters, request bodies, and response schemas.

- **Query parameters:** split from each endpoint URL and typed from example
  values (`boolean` / `integer` / `number` / `string`); always `required:
  false` (heuristic — presence in one URL cannot prove requirement).
- **Path parameters:** volatile segments (numeric ids, UUIDs, long hashes)
  are templated (`/api/users/123` → `/api/users/{id}`) with the original
  segment kept as the example.
- **Request schemas:** matching POST/PUT/PATCH form fields win (input types
  mapped, `required` honored), then JSON body hints found near the endpoint
  path in inline scripts, else a generic inferred object when a body content
  type is known. GET endpoints get no request body.
- **Response schemas:** observed bytes win — bounded host-mediated GET probes
  for GET endpoints through the owning `Session`, or bodies resolved alongside
  `scrape-endpoints` (`ResolvedBody`) — inferred into typed object/array
  schemas with a dependency-free JSON parser. POST/PUT/PATCH are **never**
  probed with their own method (side effects); their responses stay heuristic
  unless bytes were already resolved.
- **Output:** deterministic OpenAPI 3.x YAML (`x-prowsetk-schema` with
  request/response provenance plus the usual `x-prowsetk-provenance`,
  confidence, and `x-inferred` markers) and Postman 2.1 JSON (query pairs,
  `:var` path variables with values, urlencoded fields or raw JSON example
  bodies, saved example responses).

All inference is heuristic and never authoritative. Secrets (sensitive query
names, password/token JSON keys, secret-bearing form values) keep their type
but lose their example to `[REDACTED]` by default. Network access stays
host-mediated; the plugin never opens sockets and never logs secrets.

## Lua

```lua
local grabber = require("plugins.schema-grabber.lua.schema_grabber")
local scrape = require("plugins.scrape-endpoints.lua.scrape_endpoints")

local scraped = scrape.scrape(session, { url = "https://example.com/" })
local result = grabber.enrich(session, {
    endpoints = scraped.filtered_endpoints, -- optional; scraped when absent
    output = "build/openapi-enriched.yaml",
    output_postman = "build/postman-enriched.json",
})
print(result.schema_count, result.probe_count)
```

## Options

| Key | Default | Meaning |
|---|---|---|
| `url` / `html` / `base_url` | `""` | Navigation source (mirrors scrape-endpoints) |
| `endpoints` | scraped | Seed endpoints, e.g. `scrape-endpoints` output |
| `api_patterns` | `/api`, `/v1`… `/hotel/hoteladmin`, `/partner-settings`, `/telemetry`, `challenge`, `/beacon`, `/collect` | API-like path markers |
| `require_api_pattern` | `true` | Drop non-API paths (explicit non-GET methods are always kept) |
| `probe_get_responses` | `true` | GET-probe GET endpoints for response bodies (session only; same-origin unless `allow_cross_origin`) |
| `max_probe_requests` | `32` | Network probe budget (GET only, same-origin) |
| `allow_cross_origin(_resolve)` | `false` | Keep probing same-origin |
| `max_body_bytes` | `262144` | Bodies truncated before inference, marked truncated |
| `max_properties` / `max_depth` | `64` / `4` | Schema inference budgets |
| `minimum_confidence` | `0.50` | Extractor threshold |
| `openapi_version` | `"3.1.0"` | YAML version |
| `redact_secrets` / `include_provenance` / `include_examples` / `infer_schemas` | `true` | Output policy |
| `output` / `output_postman` | `""` | Optional output files |
| `collection_name` | `"Discovered API (schema-grabber)"` | Postman collection name |
