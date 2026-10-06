# Chapter 32: Lua Snapshots and API Discovery

[Manual index](README.md)

## Load the companion

`tools/qutebrowser-bridge/lua/qutebrowser_bridge.lua` connects trusted Lua drivers
to the broker and adapts captured HTML to ordinary ProwseTk interfaces. Load it
explicitly inside ProwseTk's `LuaRuntime`:

```lua
local qute = dofile('tools/qutebrowser-bridge/lua/qutebrowser_bridge.lua')
```

This path assumes the repository root as the working directory. Installed
drivers use the helper under `share/prowsetk/qutebrowser-bridge/lua/`.
The file locates its JSON helper and plugin Lua files relative to itself.

`qute.ipc(path)` loads the native `lquteipc` module. A previously loaded module
is reused. Otherwise it uses the explicit path, Lua's normal module search,
then source-tree `default`, `debug`, `asan`, and `release` build locations in
that order. Select `ipc_module` explicitly for nonstandard or sanitizer builds
so the module matches the running host.

## An offline snapshot recipe

This example runs in a ProwseTk Lua host. It constructs an inert snapshot,
queries headings, discovers API references, and writes both specifications:

```lua
local qute = dofile('tools/qutebrowser-bridge/lua/qutebrowser_bridge.lua')
local pdql = require('lpdql')
local session, browser = qute.load_snapshot{
    url = 'https://example.test/',
    html = [[
        <h1>Offline API example</h1>
        <script>fetch('/api/items/123?limit=2&active=true')</script>
        <form action='/api/items' method='post'>
          <input name='amount' type='number' required value='3'>
          <input name='customer' value='private example'>
        </form>
    ]]
}

local ok = pcall(function()
    local headings = pdql.rows(session, 'select text from xpath("//h1 | //h2")')
    assert(headings[1].text == 'Offline API example')
    local discovered = qute.scrape(session, {api_only = true})
    local enriched = qute.enrich(session, discovered.endpoints, {
        collection_name = 'Offline snapshot API'
    })
    assert(enriched.probe_count == 0)
    qute.write('build/qute-snapshot/openapi.yaml', enriched.openapi_yaml)
    qute.write('build/qute-snapshot/postman.json', enriched.postman_json)
end)
session:close()
browser = nil
if not ok then error('Snapshot extraction failed', 0) end
```

`load_snapshot` returns a managed Session and its owning Browser. Keep the
Browser alive until the Session is closed. The helper requires `html` and
`url`, enforces the 16-MiB HTML bound, and closes its Session if HTML parsing
fails. Flatworm's own node/depth limits also apply.

The created Browser disables JavaScript, redirect following, and network
observation. Loading the snapshot does not execute captured scripts or import
assistant-browser credentials. The `<script>` above remains source data for
endpoint inference. This helper creates its own Browser/Session rather than
borrowing an authenticated browser context.

See Chapter 10 for wrapping such extraction in `main(args)`, or use the shipped
offline `booking-admin-api` project in Chapter 34.

## Receive a live capture

Start the Chapter 31 broker and sender, then use the same APIs on their result:

```lua
local qute = dofile('tools/qutebrowser-bridge/lua/qutebrowser_bridge.lua')
local client = qute.connect{
    descriptor = 'build/qute-demo/bridge.json',
    ipc_module = 'build/default/tools/qutebrowser-bridge/lquteipc.so'
}
local snapshot = assert(client:snapshot(0, 30000), 'No assistant snapshot')
local session, browser = qute.load_snapshot(snapshot)
local ok = pcall(function()
    local discovered = qute.scrape(session)
    local enriched = qute.enrich(session, discovered.endpoints)
    qute.write('build/qute-demo/openapi.yaml', enriched.openapi_yaml)
end)
session:close()
browser = nil
pcall(function() client:finish() end)
if not ok then error('Assistant extraction failed', 0) end
```

The descriptor is read from an owner-only regular file. `connect` validates
its fields/version/token/origin; it does not contact Qutebrowser directly.
Without `descriptor`, it reads `PROWSETK_QUTE_BRIDGE` from the process
environment. The broker must be running for a subsequent request.

`client:snapshot(after, wait_ms)` returns the latest publication whose revision
exceeds `after`, or `nil` after a bounded wait. Defaults are 0 and 30,000 ms;
`wait_ms = 0` requests an immediate check. Reading a snapshot does not request
a new browser capture. Invoke a sender again, or use the action loop in
Chapter 33, to publish a later revision.

When the descriptor negotiates the bulk transport, the socket carries only
control metadata and the companion reads the body from a private FIFO before
returning the snapshot table. Callers see the same `{url, html, tab, purpose,
revision}` shape either way; [Chapter 39](39-snapshot-bulk-transport.md)
specifies that path.

## Companion API reference

| Function or method | Contract |
|---|---|
| `qute.connect{descriptor?, ipc_module?}` | Client for the explicit private descriptor |
| `client:status()` | Value-free broker status, revision, connection/action counters |
| `client:snapshot(after?, wait_ms?)` | Latest newer snapshot or `nil` on wait expiry |
| `client:act(action, wait_ms?)` | Fresh action capture; details in Chapter 33 |
| `client:finish()` | Close the broker conversation; returns true on success |
| `qute.load_snapshot{html, url}` | New JavaScript-disabled Session and owning Browser |
| `qute.scrape(session, {api_only?, include_noise?})` | Real scrape-endpoints Lua result; telemetry/error-reporting noise omitted by default |
| `qute.enrich(session, endpoints, {api_only?, collection_name?})` | Real schema-grabber Lua result |
| `qute.login_evidence(session, {success_selector?, success_xpath?})` | Positive-DOM login beacon used by the drivers and the console |
| `qute.challenge(snapshot)` | Advisory anti-bot triage of a captured page |
| `qute.write(path, bytes, ipc_module?)` | Atomic owner-only output |
| `qute.ipc(path?)` | Loaded native IPC/file-I/O module |
| `qute.plugin(name)` | Load a plugin Lua helper from source/install paths |
| `qute.json.decode(bytes)` / `encode(value)` | Strict bounded JSON conversion |

Recover operational errors with `pcall`. Bridge transport failures use a
generic value-free diagnostic. When a workflow owns a Client and Session,
close the Session and attempt `finish` on both success and failure.

## Compose queries and existing plugins

The Session's Document supports the same selectors, XPath, mutations, PDQL,
and IR interfaces as other loaded documents. Chapter 7 covers selectors and
XPath, Chapter 8 covers PDQL, and Chapter 22 covers IR emitters. A new HTML load
replaces the installed document; reacquire Document/Element handles afterward.

`qute.scrape` enables script inspection, all-path extraction, redaction, and
provenance. API-only filtering defaults on. It disables link following,
recursive resolution, SPA probing, and network observation. The result contains
the scraper's `endpoints`, `filtered_endpoints`, `openapi_yaml`, and
`postman_json` fields. Endpoint tables are discovery inputs; apply the export
redaction policy rather than logging their raw URL/value fields.

`qute.enrich` consumes supplied endpoints and the snapshot Document. It disables
GET response probes, sets their budget to zero, and keeps cross-origin probing
disabled. It preserves redaction/provenance and disables optional schema
examples. Its result includes `schemas`, `schema_count`, `probe_count`,
`openapi_yaml`, and `postman_json`.

The helper sanitizes a detached copy before enrichment: form-control `value`
attributes are removed and textarea content is cleared. Field names, input
types, and `required` flags remain available for inference; the caller's
Document keeps its original values. Core PDQL live queries have their own
default sanitized projection policy.

| Available evidence | Supported inference |
|---|---|
| Links, forms, inline script references | Endpoint method/path with provenance and confidence |
| Query values and volatile path segments | Typed URL parameters and path templates |
| Matching forms | Request field names/types and internal required flags |
| No HTTP response bytes | Generic heuristic response metadata only |

Form-field `required` flags are retained in the returned Lua schema records.
The current Lua OpenAPI renderer emits request properties but does not emit
the corresponding object-level `required` array; Postman request examples
likewise do not express those validation constraints. Treat the structured
records and rendered artifacts as distinct support surfaces.

The Lua helper's request enrichment uses matching forms. General nearby-script
JSON-body inference belongs to the C++ schema-grabber path described in
Chapter 20. A script mentioning an endpoint does not establish its response
schema, authentication requirements, or actual execution.

The companion exposes the positive-DOM login beacon as
`qute.login_evidence`, but it applies no policy of its own and makes no
application-wide completeness claim. `qute.challenge` only classifies a capture
through the optional captcha-handler helper; it never bypasses a challenge. The
Booking driver in [Chapter 34](34-booking-admin-api-snapshots.md) applies its own
origin, evidence, and incomplete-coverage policy, and the interactive console in
[Chapter 38](38-interactive-qute-console.md) accumulates this evidence across
captures.

## Output and lower-level IPC

`qute.write` bounds each file to 16 MiB. It writes an exclusive temporary file,
flushes it, and renames it over the destination. New parent directories use
`0700`; output files use `0600`. Existing parent directory permissions are not
changed. Multiple outputs are committed independently, not as one transaction.

The native module exposes `exchange(socket, json, timeout_ms)`,
`read_private(path)`, `write_private(path, bytes)`, and `read_fifo(path,
bytes)` for a bulk-transport body. Failures return `nil, error`; the Lua
companion raises a generic error. Exchange accepts a maximum 8-MiB single-line
JSON frame and a 1–32,000-ms transport timeout. Broker application waits remain
bounded to 30 seconds. Private reads reject symlinks, nonregular files, and
files readable by other users/groups. `read_fifo` applies the equivalent checks
to a generated FIFO basename in an owner-only directory; see
[Chapter 39](39-snapshot-bulk-transport.md).

The JSON helper rejects duplicate members, nonfinite numbers, invalid UTF-8,
malformed Unicode escapes, excessive nesting, and over-budget data. It never
evaluates the received text. `qute.json.null` represents JSON null; decoded
empty arrays retain their array shape when re-encoded.

Reference: [Lua companion](../tools/qutebrowser-bridge/lua/qutebrowser_bridge.lua),
`tools/qutebrowser-bridge/ipc.cpp`, `tests/unit/test_qutebrowser_bridge.cpp`.

**Next:** [Two-way Qutebrowser marionettes](33-qutebrowser-marionettes.md).
