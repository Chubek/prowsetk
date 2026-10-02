# Chapter 16: scrape-endpoints

[Manual index](README.md)

## Purpose and first use

scrape-endpoints combines endpoint discovery, API-only filtering, optional
resolution, SPA interactions, and OpenAPI/Postman export. Its Lua helper runs
inside the owning managed Session:

```lua
local scrape = dofile('plugins/scrape-endpoints/lua/scrape_endpoints.lua')
local result = scrape.scrape(session, {
    html = '<a href="/api/items?page=2">Items</a>',
    base_url = 'https://example.test/',
    minimum_confidence = 0.40,
    spa_probe = false,
    resolve_chain = false
})
assert(result.endpoint_count == 1)
local yaml, postman = result.openapi_yaml, result.postman_json
```

Use `url` to navigate a live source, or omit both source fields to operate on
the current document. `html` supplies an offline document; disable resolution
and interaction for a deterministic static extraction. Lua result fields are
values, unlike the built-in extractor's getter methods.

## Key Lua options

| Field | Default | Effect |
|---|---|---|
| `url`, `html`, `base_url` | empty | Source loading/base resolution |
| `api_only` | true | Remove non-API/garbage paths before exports |
| `require_api_pattern` | true | Require API-like GET paths; keep explicit non-GET methods |
| `api_patterns`, `garbage_patterns` | built-in lists | API markers and asset exclusions |
| `scrape_all_paths` | false | Broaden initial discovery |
| `spa_probe`, `max_spa_actions` | true, 32 | Bounded synthetic actions before extraction |
| `resolve_chain` | false | Resolve selected endpoints through host requests |
| `follow_json_links` | true | Follow API-like JSON references during resolution |
| `max_depth`, `max_pages` | 2, 100 | Resolution/traversal-related bounds |
| `max_resolve_requests` | 64 | Resolution request budget |
| `allow_cross_origin_resolve` | false | Lua resolution origin policy |
| `output`, `output_postman` | empty | Optional YAML and collection files |
| `minimum_confidence`, `openapi_version` | 0.50, 3.1.0 | Discovery and output settings |

Core inspection, observation, inference, provenance, and redaction booleans
default true. C++ ScrapeEndpointsOptions has the documented counterpart fields;
consult its header for differences from the Lua table.

SPA probing targets buttons, submit controls, role-button elements, and
JavaScript/hash links. It executes real page handlers through the synthetic
driver, so it can produce network operations and navigation. `spa_probe = false`
selects inspection without those actions.

## Filtering and output

With API-only enabled, static assets, bundles, images, fonts, media, and
JavaScript function/arrow-expression paths are removed, including recursively
discovered paths. Ordinary query values do not trigger the path-code filter.
`require_api_pattern = false` retains ordinary non-asset paths;
`api_only = false` bypasses the API-only filtering altogether.

Lua result fields include `endpoints` (resolved/export entries), `all_endpoints`,
`filtered_endpoints` (plain discovered endpoints), `warnings`, `endpoint_count`,
`openapi_yaml`, `postman_json`, and optional `assistant_browser` metadata.
`write_openapi_yaml(path)` and `write_postman_json(path)` write artifacts.
Use `filtered_endpoints` when seeding schema-grabber. Shared render/export
helpers support merged endpoint tables in crawler and Booking workflows.

Both exports retain provenance/confidence and redact private URL components,
including encoded sensitive query names, userinfo, and provenance/redirect URLs.
Resolved body bytes are raw intermediate inputs; do not print them as diagnostics.

## Assistant-browser handoff

`assistant_browser_enabled` defaults false. When enabled, challenge heuristics
or an empty discovery can produce handoff metadata: reason, URL, command,
method, endpoint, and debug port. `assistant_prompt` defaults true.
`$PROWSETK_ASSISTANT_BROWSER` overrides the command, and
`assistant_wait_timeout_ms` defaults to 300,000.

The Lua helper can prompt and launch the configured browser; C++ detection
returns a handling description. A handoff alone does not transfer cookies,
prove login, or prove endpoint coverage. A surrounding driver/host must obtain
fresh data explicitly and confirm the resulting session. Crawler's supervised
argv-based assistant contract and Beacon's Firefox data delivery are described
in Chapters 24 and 27.

## Native and C++ integration

The plugin shared object is `libprowsetk_scrape_endpoints.so` on Linux. Load and
initialize it through PluginRegistry; configure native output keys with
`scrape-endpoints.output_openapi` and `scrape-endpoints.output_postman`.
The native document hook consumes a snapshot and produces document-only
exports. It has no live Session for SPA probing or network resolution. For
those paths, call `scrape_from_session`/`scrape` in C++ or the Lua helper.

Earlier `scrape2oapi`/`scrape2postman` trees live under `plugins/.deprecated/`.
New work uses this unified module. Compatibility C++ aliases remain in its
header; replace old Lua module paths and configure both outputs on the same
result when migrating.

Reference: `plugins/scrape-endpoints/`,
`plugins/scrape-endpoints/include/prowsetk/plugins/scrape_endpoints.hpp`.

**Next:** [ezlogin](17-ezlogin.md).
