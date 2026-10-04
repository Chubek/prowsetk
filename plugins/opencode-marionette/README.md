# opencode-marionette

An explicit synchronous OpenCode controller for a Flatworm Session. It composes
`opencode-bridge`, `scrape-endpoints`, and `schema-grabber`; no new library,
WASM toolchain, or graphical browser is required.

Build with the normal default preset. The shared native ABI-v2 library has a
network-free lifecycle facade; loading it alone does not drive pages. Use the
C++ `opencode_marionette::run(session, client, decisions)` API or the shipped
`ptk-opencode-marionette` executable to start an explicit run. The plugin ABI
has no session-control service, so the controller does not smuggle C++ objects
across it. No Lua API is added.

## Running

Start an OpenCode API server separately (`opencode serve`). Set
`OPENCODE_BASE_URL` if it differs from `http://127.0.0.1:4096`, and use
`OPENCODE_SERVER_USERNAME`/`OPENCODE_SERVER_PASSWORD` for Basic authentication.
Set the username to `opencode` when the server uses that default. Authentication
is isolated from page cookies and headers; do not put credentials in decisions.
The documented synchronous `/session/:id/message` text-parts protocol is used.
Servers needing the bridge's older `/api/.../prompt` polling protocol are not
supported by this runner. Transport redirects are forbidden; remote plain HTTP
is disabled by default. Requests, response bytes, and timeouts inherit bounded
OpenCode bridge limits. No server is spawned by the plugin.

```sh
build/default/plugins/opencode-marionette/ptk-opencode-marionette \
  plugins/opencode-marionette/decisions.example.json \
  https://your-site.example/ api.yaml postman.json
```

An optional final HTML_FILE argument loads HTML instead of the initial page
navigation. This still contacts OpenCode and can issue page requests and schema
probes; hermetic runs use MemoryNetworkClient transports in the C++ API/tests.

## JSON decisions

`decisions.example.json` is a trusted version-1 policy, capped at 64 KiB:

- `goal`: nonempty string (at most 4096 bytes). Sent to OpenCode; keep it secret-free.
- `max_steps`: 1–64 (default 16).
- `max_page_requests`: 1–1024 (default 128), shared across actions, script calls,
  redirects and probes during `run`. Initial document loading is caller-owned.
- `max_get_probes`: 0–128 (default 32), shared across all visited documents.
- `actions`: up to 128 entries. Each has a unique ASCII `id`, `kind`, and
  optional `max_uses` (1–64, default 1). `click` requires a CSS `selector`;
  `type` requires `selector` and trusted `value` and appends text through the
  synthetic keyboard cascade. `navigate` requires `url` (relative or same-origin).

Unknown keys, duplicate keys, invalid kinds/limits and extra action parameters
fail before browser interaction. OpenCode must return only `{"action":"ID"}`
or `{"action":"stop"}`. Unknown/exhausted actions and malformed responses fail;
model output never becomes JavaScript, Lua, shell commands, selectors or values.
Use descriptive IDs: OpenCode receives IDs, action kinds, target presence/tag,
availability attributes and endpoint counts. It receives no page text, HTML,
private form values, scripts, cookies, headers, selector strings or typing values.
The current URL receives default URL redaction. This intentionally narrow page
context works with caller-defined workflows rather than arbitrary site browsing.

Only explicitly permitted actions run. Automatic scrape-endpoints SPA probing
is disabled. Click/type still trigger the Flatworm bounded event/microtask/network
cascade. All page requests during `run` stay on the starting origin; cross-origin
requests and redirects fail the run, including requests initiated by scripts.
Existing browser plugins and host transport remain trusted. Run on the owning
session thread; concurrent/reentrant calls on the same session are unsupported.

## Extraction and limits

Discovery runs before and after every action, accumulates across navigation,
filters static assets with scrape-endpoints, and enriches same-origin results
through schema-grabber. GET response probes are deduplicated and bounded; POST,
PUT and other methods are never probed. Their schemas remain inferred unless
available from document hints; the current Session event interface does not
expose request/response bodies. Schema examples are omitted from OpenAPI and
secret-bearing fields receive existing schema-grabber redaction. Exported data
contains provenance/confidence and an explicit incomplete coverage warning.
The OpenAPI `x-prowsetk-marionette` block records action count and stop reason.
A normal result after exhausting steps has `stopped=false` and `reason=step-limit`.
An agent stop never establishes complete endpoint coverage. Flatworm retains
its documented partial JavaScript compatibility and layout-free interactions.
No authentication or CAPTCHA bypass is provided.

The C++ result provides the combined schemas, OpenAPI YAML, Postman JSON, probe
count, warnings, steps and stop reason. Errors retain ErrorCode with generic,
value-free messages; failed runs do not return a partial result. Event hooks
are removed on both success and failure. CLI output is written only after a
successful run; output-write errors produce a nonzero exit code.

Tests are under `tests/unit/test_opencode_marionette.cpp` and
`tests/integration/test_opencode_marionette.cpp`, registered with CTest labels
and finite timeouts, using separate in-memory OpenCode and page transports.
