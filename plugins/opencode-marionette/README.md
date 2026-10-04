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
The runner defaults to OpenCode V2: `POST /api/session`,
`POST /api/session/:id/prompt`, and bounded polling of the newest
`GET /api/session/:id/message` records. Decision sessions install a deny-all
tool permission policy; only the host executes policy-approved page actions.
The C++ API selects this protocol when the client's `api_prefix` is `/api`;
an empty prefix retains the bridge's synchronous text-parts protocol.
Transport redirects are forbidden; remote plain HTTP
is disabled by default. Requests, response bytes, and timeouts inherit bounded
OpenCode bridge limits. No server is spawned by the plugin.

```sh
build/default/plugins/opencode-marionette/ptk-opencode-marionette \
  plugins/opencode-marionette/decisions.example.json \
  https://your-site.example/ api.yaml postman.json
```

An optional HTML_FILE argument (before named options), or `--html HTML`, loads
an offline page through an in-memory page transport instead of navigating.
GET schema probes are disabled for offline pages. This still contacts OpenCode;
fully hermetic C++ runs/tests supply in-memory transports for both clients.
`--max-steps`, `--max-page-requests` and `--max-get-probes` override policy
budgets within the same hard bounds. `--opencode-max-requests` (default 512,
polls included) and `--opencode-wait-ms` (default 300000 per reply) bound V2 IPC.
The output parent directories are created after successful exploration.
`--verbose`/`-v` (`--verbse` alias) adds value-free stage, network, proxy,
cookie-import, timing and page-runtime diagnostics. Failures always identify
the stage and `ErrorCode`; controller failures distinguish policy validation,
schema collection, OpenCode session/decision calls, page actions and serialization.
Network traces show only hosts, routing, cookie presence and numeric metadata;
page bodies, URL queries, cookies, proxy credentials and raw agent/script errors
are omitted. Environment-selected proxies honor the target scheme's
`HTTPS_PROXY`/`HTTP_PROXY` (and lowercase aliases) plus `NO_PROXY`/`no_proxy`.

### Booking launcher

```sh
scripts/run-scrape-booking.sh \
  --cookies-json _scraped/booking-dotcom-admin/cookies.json \
  --output _scraped/booking-dotcom-admin/api.yaml \
  --postman _scraped/booking-dotcom-admin/api.postman_collection.json
```

The launcher starts/authenticates an OpenCode V2 server, then invokes this
controller with `--booking-config examples/booking-dotcom-admin-scrape/Prowse.toml`.
It reuses the driver's `prepare_session(args)` through `LuaRuntime::bind_session`;
login, imported cookies, the Firefox handoff and positive DOM confirmation occur
on the very session subsequently controlled by C++. No plugin Lua module is
introduced. The existing `prowsetk run booking-dotcom-admin` driver remains
available for its crawl/cleanup workflow.

The default trusted `marionette-decisions.json` beside that config covers
navigation links, read-only tabs, reservation-detail expansion and pagination.
Its descriptive action IDs and goal guide the agent toward distinct API views.
Use `--decisions FILE` to match your current page's selectors. Only listed actions
can run; the starting policy does not establish that every live Booking UI has
those controls. Its defaults are 24 actions, 512 page requests and 64 GET probes.
The launcher retains the forced Firefox handoff, Joe Litty Rooms XPath login
beacon and booking.com-only export defaults. Login options and explicit booleans
are forwarded to the runner; `--no-xcors false` disables only the output host
filter, while the controller's same-origin transport rule remains in force.
`PROWSETK_MARIONETTE_BIN`/`--marionette-bin` selects the executable, and
`--no-server` reuses `OPENCODE_BASE_URL`. `--help` lists the supported options.
The launcher adds loopback proxy exclusions for the local agent and retains
its owner-only server log on failure or verbose runs. Firefox cookie capture
considers main-database and WAL freshness; set `FIREFOX_PROFILE_DIR` to the
browser's exact profile to override selection. Capture/import failures are
reported before network confirmation, and a forced browser handoff occurs only
once per preparation. Login diagnostics report origin, account-control, beacon,
password-field and challenge evidence without the page's values.

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
through schema-grabber. GET response probes are deduplicated and bounded;
logout/sign-out-like paths stay discovered but are skipped by automatic probes
to preserve the authenticated session. POST, PUT and other methods are never
probed. Their schemas remain inferred unless available from document hints;
the current Session event interface does not
expose request/response bodies. Schema examples are omitted from OpenAPI and
secret-bearing fields receive existing schema-grabber redaction. Exported data
contains provenance/confidence and an explicit incomplete coverage warning.
The OpenAPI `x-prowsetk-marionette` block records `used: true`, action count,
GET probe count and stop reason. Booking preparation adds
`x-prowsetk-booking.authenticated` (false for offline HTML).
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
`tests/integration/test_booking_marionette.py` exercises the real launcher and
runner against loopback-only V2/page fixtures, including login, clicks, navigation,
typed schemas, redaction, offline exports and server cleanup.
