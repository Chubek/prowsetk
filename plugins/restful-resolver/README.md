# restful-resolver

Native ProwseTk plugin that resolves scraped endpoints iteratively until a
full RESTful API surface is discovered.

- **Seed:** static discovery through `EndpointExtractor` (links, POST forms,
  `fetch`/`XMLHttpRequest` methods including `POST`, `navigator.sendBeacon`,
  shorthand POST helpers such as `$.post` / `axios.post` / `$.ajax`, and
  generic options-object POST configs such as `axios.request({method, url})`
  or `fresa({uri, method})`).
- **Resolve:** breadth-first GET probes through the owning `Session`
  (`Session::request`, host-mediated `NetworkClient`). JSON bodies that
  reference further API URLs are enqueued; HTML bodies are re-parsed so POST
  forms and script POST calls in resolved pages are discovered.
- **Stop:** as soon as at least one GET **and** one POST endpoint are known
  (`is_complete`), or when `max_rounds` / `max_requests` is exhausted.
- **Output:** deterministic OpenAPI 3.x YAML with the `x-prowsetk-restful`
  extension (`has-get`, `has-post`, `is-complete`, `rounds-used`,
  `request-count`), provenance, confidence, and secret redaction.

Discovery is heuristic and never authoritative. Network access stays
host-mediated; the plugin never opens sockets and never logs secrets.

## Lua

```lua
local restful = require("plugins.restful-resolver.lua.restful_resolver")
local result = restful.resolve(session, {
    url = "https://admin.booking.com/",
    endpoints = seed_endpoints, -- optional
    max_rounds = 4,
    max_requests = 64,
})
print(result.is_complete, result.has_post, #result.endpoints)
```

## Options

| Key | Default | Meaning |
|---|---|---|
| `url` / `html` / `base_url` | `""` | Navigation source (mirrors scrape2oapi) |
| `api_patterns` | `/api`, `/v1`… `/hotel/hoteladmin`, `/partner-settings`, `/telemetry`, `challenge`, `/beacon`, `/collect` | API-like path markers |
| `require_api_pattern` | `true` | Drop non-API paths (explicit non-GET methods are always kept) |
| `max_rounds` | `4` | BFS round budget |
| `max_requests` | `64` | Network fetch budget |
| `follow_json_links` | `true` | Enqueue API URLs found in bodies |
| `allow_cross_origin(_resolve)` | `false` | Keep resolution same-origin |
| `minimum_confidence` | `0.45` | Extractor threshold |
| `openapi_version` | `"3.1.0"` | YAML version |
| `redact_secrets` / `include_provenance` / `infer_schemas` | `true` | Output policy |
