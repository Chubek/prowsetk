# Booking.com admin API discovery with Qutebrowser

This `Prowse.toml` project uses the
[Qutebrowser userscripts](../../tools/qutebrowser-bridge/README.md) as its assistant
browser. Log in interactively in Qutebrowser, send the current admin DOM to Lua,
and generate redacted OpenAPI 3.1 and Postman 2.1 specifications with the real
scrape-endpoints and schema-grabber plugins. Optional trusted actions turn the
same driver into a two-way marionette.

## Interactive session (recommended)

```sh
cmake --preset default
cmake --build --preset default
examples/booking-dotcom-admin-api/qute-assist.exp --directory build/qute-booking
```

This now starts a **Replxx-backed Lua REPL**, with editing, tab completion,
hints, multiline Lua and memory-only history. Run its printed
`ptk-qute-marionette` command in the already-logged-in admin tab. At `qute>`:

```text
:capture
:status
:links
:targets
:click 2
:forms
:fill 4 "2027-01-01"
:select 5 "confirmed"
:check 6 true
:scroll 7
:endpoints
:export
```

Numbers above are illustrative: choose a target from the current inspection
list. A capture invalidates those numbers; inspect again before another
numbered action. `:focus`, `:submit`, `:navigate /relative/path`, `:reload` and
`:help` provide more orders. Each browser order returns a fresh capture.
Use actual application sections, reservation details, filters, pagination and
lazy-load controls, rather than capturing only the account menu.

You can enter Lua directly and retain globals/functions between responses:

```lua
qute:click("button[role='tab'][aria-controls='reservations']")
qute:fill("input[name='date']", "2027-01-01")
qute:beacon("[data-testid='account-menu']")
require('lpdql').rows(session, 'select tag, text from <h*>')
for _, ep in ipairs(qute.endpoints) do print(ep.method, ep.path) end
```

`session` and `document` refer to the current JavaScript-disabled snapshot.
Host Lua is trusted, synchronous automation; page/model output is never
executed. Explicit Lua prints can display private page values. Built-in orders
omit labels/form values by default; `:values on` or `--show-values` opts into
local labels. History is never written to disk. `:quit` finishes the
conversation and the launcher reaps its broker, leaving Qutebrowser open.

The console accumulates same-origin endpoints and sanitized **per-page** form
schemas through the existing scrape-endpoints and schema-grabber plugins. Later
captures merge query names and form fields by method/templated path, producing
one operation with an evidence count instead of duplicate OpenAPI keys. Error-reporting and
telemetry candidates such as `/js_errors` are filtered, and `:export` refuses an
empty result instead of replacing useful artifacts with noise-only specs.
Exports happen only on `:export`, use the configured `_scraped/` destinations,
and remain redacted, heuristic and incomplete. DOM captures do not contain
external bundles, historical network requests or response bodies, so response
schemas cannot be verified through this interface.

HTML now streams through owner-only, length-framed **FIFOs on both hops**, from
the userscript to the broker and from the broker to Lua. The old 4 MiB HTML cap
and JSON-escaping overhead are removed; the current bound is **16 MiB per
capture**, with Flatworm's node/depth bounds still applying. Transfers handle
backpressure, truncation and peer failure with finite deadlines. Tokens and
page bodies are never put in Qutebrowser commands.

The default session lasts 1,800 seconds with at most 256 actions; override with
`--timeout` (1–3600) and `--max-actions` (0–256). Use `--repl-bin` for another
preset's `ptk-qute-repl`. Replxx is optional at build time
(`PROWSETK_BUILD_QUTE_REPL`); `--one-shot` retains the workflow below.

Launcher options are split by the consumer they configure:

| Option | Consumer | Meaning |
|---|---|---|
| `--settle-ms N` | either | Marionette capture delay in the printed `:spawn` line, 0–5000 |
| `--show-values` | console | Display target labels locally |
| `--include-noise` | console | Keep telemetry/error-reporting candidates |
| `--max-actions N` | either | Interaction budget, 0–256 |
| `--api-only BOOL` | finite driver | `false` keeps every discovered same-origin candidate |
| `--actions-file F` | finite driver | Trusted action policy; also selects `--one-shot` |

Noise filtering is on by default in both launchers' downstreams, so
`/js_errors` and similar paths no longer reach an export unless you ask for
them. In the console use `--include-noise`; in the finite driver use
`--api-only false`.

## One-shot workflow

From the repository root:

```sh
cmake --preset default
cmake --build --preset default
tools/qutebrowser-bridge/ptk-qute-bridge serve \
  --directory build/qute-booking --url https://admin.booking.com/ --timeout 300
```

In a second terminal:

```sh
build/default/src/cli/prowsetk run booking-admin-api \
  --config examples/booking-dotcom-admin-api/Prowse.toml \
  --bridge "$PWD/build/qute-booking/bridge.json" \
  --output _scraped/booking-admin-openapi.yaml \
  --postman _scraped/booking-admin-postman.json
```

The driver waits up to 30 seconds for a snapshot. From the logged-in admin tab,
run (substitute absolute paths):

```text
:spawn --userscript /absolute/prowsetk/tools/qutebrowser-bridge/ptk-qute-scrape --bridge /absolute/prowsetk/build/qute-booking/bridge.json
```

You can send the snapshot before starting the driver; the broker retains its
latest capture until the conversation ends or its lifetime expires. Restart
the broker for a new run. `serve --launch` can open Qutebrowser after approval.

Use your existing logged-in admin tab; another login is not required. Live
exports require positive DOM account/logout evidence. Without an override, the
driver recognizes `[data-testid='account-menu']`, Log out / Sign out / Log off /
Sign off link or button labels (including nested spans and accessible labels),
and matching logout path components in links or form actions. An explicit
`success_selector` or `success_xpath` overrides these heuristics; for example,
`--success_xpath "//h1[contains(normalize-space(.), 'Your property')]"`.
Words inside scripts do not count as evidence. MFA/human verification happens
in the assistant browser. No credentials are accepted or logged by this driver.

Qutebrowser userscripts do not report HTTP status or session credentials. The
metadata therefore distinguishes **positive DOM login evidence** from verified
authentication: `authentication-verified: false`. Sending HTML never imports a
logged-in cookie jar into ProwseTk, and the driver makes no live page requests.

## Two-way marionette

Copy `actions.example.json` to a local policy and replace its selector with an
actual non-destructive tab/navigation control on your admin page. The sample
selector is illustrative, not a claim about Booking.com's current DOM.
Policies are trusted JSON with `version: 1` and at most 32 `actions`.

```json
{"version":1,"actions":[
  {"type":"click","selector":"button[role='tab'][aria-controls='reservations']"}
]}
```

Start a fresh broker, add `--actions_file /absolute/my-actions.json` to the driver
command, and connect from Qutebrowser with:

```text
:spawn --userscript /absolute/prowsetk/tools/qutebrowser-bridge/ptk-qute-marionette --bridge /absolute/prowsetk/build/qute-booking/bridge.json
```

Lua collects the initial endpoints, requests each action, then parses the fresh
HTML returned by the browser. It checks same-origin and DOM login evidence on
each capture, merges discovery, and exports both specifications. Missing/hidden/
disabled targets return a generic error; timed-out actions are not retried.
Keep the connected tab active and do not reorder tabs. The script exits when
the driver finishes the conversation; the browser stays open.

Discovery stays on `https://admin.booking.com` and API-only filtering is enabled
by default. `--api_only false` broadens extraction to ordinary same-origin
paths. Anchor `href` values are harvested from the captured markup without any
request, so an API-shaped link counts as evidence with provenance `html-link`;
link *following*, recursive resolution and SPA probing stay disabled. A link is
something the page offers, not a call the application is known to make. Use the
console's navigation orders to capture the pages those links point at, since
each capture contributes its own forms and inline scripts. Schema enrichment
uses the final capture's forms/scripts; endpoints
from earlier captures are retained but may have only generic request hints.
Response bodies are unavailable, so no GET probes or observed response-schema
claims are made. Coverage is explicitly incomplete in both artifacts.
Form values are removed before schema enrichment; names, types and required
flags remain. The caller's captured DOM is retained for querying.

## Anti-bot and CAPTCHA handling (no bypass)

The driver does not bypass CAPTCHAs, rate limits, or WAF challenges. When
`plugins/captcha-handler/lua/captcha_handler.lua` is present beside the checkout,
each snapshot is triaged with its heuristic `inspect_response` classifier
(reCAPTCHA/hCaptcha/Turnstile markers, human-verification text, 403/429 status
signals). Detection is heuristic with provenance/confidence, never authoritative
proof, and the diagnostic on stderr is value-free: it names only the inferred
category, never page text, cookies, or tokens.

The supported handling path is the assistant browser you already run:

1. Solve the challenge interactively in the Qutebrowser tab (login, MFA,
   human verification). This is the manual-user-prompt / wait-for-clearance
   plan from `plugins/captcha-handler`; solver keys and clearance cookies are
   never logged or exported.
2. Send a fresh snapshot with `ptk-qute-scrape` (or `ptk-qute-marionette`).
3. The driver re-checks same-origin scope and positive DOM login evidence
   before exporting. A snapshot without login evidence still fails with the
    stage-specific `login evidence failed` diagnostic and preserves
   previous artifacts.

If the helper is absent, the driver keeps working: the login-evidence gate is
unchanged. No solver API, webhook, pre-solved token, or cookie import is
configured by this example.

## OpenCode marionette (`plugins/opencode-marionette`)

`marionette-decisions.json` is a trusted version-1 OpenCode policy for
read-only admin views (reservation/availability/rates/reviews tabs, next-page
pagination, reservation details, same-origin `/reservations` and
`/availability` navigation). Selectors are illustrative starting points; match
them to the current admin DOM. `max_get_probes` is 0 because snapshot sessions
carry no response bodies.

Workflow: capture a logged-in snapshot in Qutebrowser (solving any challenge
there first), save its HTML to a file, then let OpenCode choose among the
allow-listed IDs:

```sh
# Start an OpenCode server separately, e.g. `opencode serve`, then:
examples/booking-dotcom-admin-api/run-opencode-marionette.sh \
  --snapshot /absolute/snapshot.html \
  --output _scraped/booking-admin-marionette-openapi.yaml \
  --postman _scraped/booking-admin-marionette-postman.json
```

The runner calls `ptk-opencode-marionette DECISIONS URL OPENAPI POSTMAN SNAP.html`
(build it with the default preset; override with `PROWSETK_MARIONETTE_BIN` or
`--marionette-bin`). Behavior follows the plugin contract: OpenCode receives
only action IDs, kinds, target presence/tag/availability and endpoint counts --
no page text, HTML, form values, scripts, cookies, headers, selectors or typing
values. Only listed actions run inside a bounded same-origin session; logout-like
paths stay discovered but are never auto-probed, non-GET endpoints are never
probed, exports are redacted, and coverage is explicitly incomplete
(`x-prowsetk-marionette` with `used: true`). A separate optional subtractive
cleanup pass is available through `plugins/opencode-bridge`
(`lopencode.build_cleanup_prompt`): the agent may only drop entries, and the
caller intersects the answer with the scraped `(method, url)` set so invented
URLs can never enter the specs.

## Remote Chromium snapshots (`plugins/browser-run-integration`)

When the admin page needs full Chromium rendering beyond Flatworm's partial
page-JS support, `ptk-browser-run content` (Cloudflare hosted browser) can
supply the snapshot instead of Qutebrowser. The snapshot then flows through the
same pipeline unchanged — `prowsetk run booking-admin-api --html`, or
`run-opencode-marionette.sh --snapshot`:

```sh
export CLOUDFLARE_ACCOUNT_ID=<32-char-hex-account-id>
export CLOUDFLARE_API_TOKEN=<token-with-Browser-Rendering-Edit>
examples/booking-dotcom-admin-api/fetch-browser-run-snapshot.sh \
  --output _scraped/booking-admin-browser-run.html
```

How it can be leveraged, and its limits:

- **Same downstream, same gates.** The fetched HTML is an explicit snapshot:
  import it into the JavaScript-disabled Flatworm Session (the `--html` path
  already builds `javascript=false`), then scrape-endpoints + schema-grabber run
  exactly as with assistant-browser captures. No cookies, JavaScript state,
  HTTP status, or response bodies transfer with the import; the positive DOM
  login-evidence check still applies, `authentication-verified` stays `false`,
  and redaction/incomplete-coverage metadata are unchanged.
- **Credentials stay control-plane.** The Cloudflare token is separate from
  page Sessions: it travels only as a Bearer header to the fixed
  `https://api.cloudflare.com/client/v4/accounts/{id}/browser-run` origin
  (never in page URLs or Session headers). Environment values override the
  `[cloudflare]` table in `Prowse.toml`; with neither token source the client
  falls back to the Cloudflare-bound oauth-assist cache, which must itself
  carry the Browser Rendering permission. Never commit a real token.
- **Remote traffic is remote.** Local Session request hooks do **not**
  intercept the remote Chromium's page requests; shape remote network/action
  policy through Cloudflare/CDP if needed. The anti-bot triage above still
  applies to the imported snapshot, and challenges are satisfied in the remote
  session by the caller before re-snapshotting — this plugin provides no
  CAPTCHA bypass.
- **CLI scope.** `ptk-browser-run` is a single-shot client: `content` uses
  Quick Actions navigation/load defaults, `cdp` sends one command per call
  (use `--browser-session` to reconnect; explicitly issue `Browser.close`
  when finished, since CLI disconnect does not close the remote browser).
  Multi-command automation (create page, attach, drive Page/Runtime/DOM/Input
  on one connection) is the C++ `browser_run::Client` API, and it is not a
  transparent remote backend for the existing Lua/OpenCode controllers —
  compose at the snapshot level instead.
- `--output` writes caller data that may contain private page values: on POSIX
  it creates a new `0600` file and refuses existing ones (remove or rotate a
  previous snapshot first). Loading the native facade alone is network-free;
  every remote call is explicit.

## Assisted runs with minimal input (`expect`)

Two `expect` scripts orchestrate the workflows above so you type as little as
possible. Neither fabricates consent or logs secrets: the Qutebrowser login and
any `--launch` approval stay human, and the Cloudflare token is asked for only
when the environment carries neither credential, read with echo disabled and
passed to children via the environment (never argv, logs, or specs).

```sh
# Qutebrowser interactive Lua: starts the broker, prints one marionette
# :spawn line, then accepts inspection/navigation/Lua in the terminal.
examples/booking-dotcom-admin-api/qute-assist.exp \
  --directory build/qute-booking --timeout 1800
# Add --one-shot for the finite scrape-and-export workflow.

# Cloudflare snapshot-to-specs: prompts for missing credentials only, fetches
# the rendered snapshot, then runs the driver on it (needs nothing else).
CLOUDFLARE_ACCOUNT_ID=... CLOUDFLARE_API_TOKEN=... \
  examples/booking-dotcom-admin-api/browser-run-assist.exp \
  --output _scraped/booking-admin-browser-run.html
```

- `qute-assist.exp` defaults to the Lua console above. `--one-shot` wraps the
  finite driver workflow (`--actions-file` also selects the driver and switches the
  printed line and driver to the two-way `ptk-qute-marionette` round-trip;
  `--launch` relays the broker's own `[y/N]` approval to your keystroke
  instead of answering it). In driver mode, exit is the driver's exit: `0` on exported specs,
  `1` on bridge/driver failure. It waits for the first capture before starting
  the driver. If that capture lacks login evidence, it keeps the broker open
  and waits for a newer capture within the same bounded login window; already
  dispatched actions are never retried. Failed validation before export
  preserves previous artifacts; the two output writes are independent.
  `--settle-ms` sets the delay in the printed marionette line, so a slow admin
  page can be given longer without editing the command by hand.
- `browser-run-assist.exp` wraps `fetch-browser-run-snapshot.sh` and chains
  the result: `--to driver` (default, offline `--html` import; snapshots over
  512 KiB are refused because the markup travels as an argv argument, with a
  pointer to `--to marionette`, which reads the snapshot file directly up to the
  16 MiB snapshot bound but needs a running OpenCode server). Its
  `--api-only false` keeps every discovered same-origin candidate. For
  `--to marionette` it also forwards `--verbose`, `--no-xcors`, `--max-steps`,
  `--max-page-requests`, `--opencode-max-requests`, `--opencode-wait-ms` and
  `--opencode-api-prefix`, and says so once if they are given with `--to driver`.

## OpenCode marionette over a snapshot

`run-opencode-marionette.sh` feeds a captured snapshot to the OpenCode agent,
which may only choose among the allow-listed action IDs in
`marionette-decisions.json`:

```sh
tools/qutebrowser-bridge/ptk-qute-repl ...          # capture, then :capture
examples/booking-dotcom-admin-api/run-opencode-marionette.sh \
  --snapshot /absolute/path/to/snapshot.html \
  --verbose --max-steps 24 --opencode-api-prefix /api \
  --output build/qute-booking/openapi.yaml \
  --postman build/qute-booking/postman.json
```

This is the offline, JavaScript-disabled path: the page transport is
deterministic and the runner forces GET schema probes to zero, while the
separate OpenCode client stays live. It needs a running OpenCode server
(`opencode serve`). It is not the login path — that is
`scripts/run-scrape-booking.sh`, which drives a live authenticated session.

The wrapper forwards the runner's snapshot options, and every value reaches the
runner as exactly one argument, so a selector or beacon containing spaces is not
re-split or pathname-expanded:

| Option | Meaning |
|---|---|
| `--verbose` | Value-free stage, request, proxy and login diagnostics |
| `--no-xcors BOOL` | Booking.com-only export filter; omitted keeps the runner default |
| `--max-steps N` | Marionette action budget |
| `--max-page-requests N` | Marionette page-request budget |
| `--opencode-max-requests N` | Agent request budget, polls included |
| `--opencode-wait-ms MS` | Cap on waiting for one agent reply |
| `--opencode-api-prefix /api` | OpenCode V2 route prefix |

`--booking-config`, `--cookies-json`, `--dotenv`, `--success-beacon`,
`--success-beacon-type`, `--assistant-browser-force` and `--max-get-probes`
configure live Booking login preparation. They are accepted only together with
`--booking-config` (which requires a project whose driver is named
`booking-dotcom-admin`), because they have no effect without it; supplying them
alone is a usage error rather than a silently ignored flag.

Unknown options, malformed values and login options used alone exit `2` before
anything is spawned. Budget ranges belong to the runner, so an out-of-range value
fails there instead of duplicating its caps in shell.
- Both scripts default every path/URL/timeout (`--help` lists overrides) and
  exit `2` on usage errors, including a non-boolean `--api-only`. `qute-assist.exp` checks the exact admin origin and
  selects the IPC module beside the chosen CLI build when available; the console
  instead resolves its own adjacent module, so `--repl-bin` and `--cli-bin` may
  point at different presets without mixing them.
- `qute-assist.exp` ensures the broker directory is owner-only (`0700`) before
  starting the broker — a hand-made directory with wider permissions would
  otherwise fail with the broker's generic error. A live broker for the
  directory is never evicted; only provably stale state is cleared.

### Logged in, but the export still fails?

In `--one-shot` mode, `snapshot received` means Qutebrowser delivered the HTML successfully. The
driver now reports the failed stage instead of just `operation failed`:

- **login evidence:** Open the account menu in your already-logged-in tab so
  its logout control is in the captured DOM, and run the printed capture
  command again. `qute-assist.exp` keeps waiting for that newer capture. For
  two-way runs it prints a one-shot `ptk-qute-scrape` command, retaining the
  already-connected marionette. The implicit
  check matches only actual controls and their logout paths/labels: script,
  style and template content, ordinary page prose, and query values such as
  `redirect=logout` are excluded. Custom CSS/XPath beacons take precedence.
- **IPC module loading:** Build the CLI's preset or use `--ipc-module` with
  the matching `lquteipc.so`.
- **snapshot parsing:** The capture must fit the bridge's 16 MiB HTML limit
  and Flatworm's 250,000-node / 256-level parser bounds.
- **endpoint extraction / schema enrichment:** Check the named Lua plugin
  helper and the extraction bounds.
- **OpenAPI export / Postman export:** Check the output parent directories
  and write permissions.

Diagnostics use fixed stage names and hints; page values, selectors, private
paths from exceptions, credentials, and raw Lua exceptions are not printed.

If the implicit controls do not match your UI, choose an authenticated-only
control in the current DOM and pass it through:

```sh
examples/booking-dotcom-admin-api/qute-assist.exp --directory build/qute-booking \
  --success-selector "a[href*='signout']"
# or an XPath beacon instead:
#   --success-xpath "//h1[contains(normalize-space(.), 'Your property')]"
```

The same two flags exist on the raw driver as `--success_selector` /
`--success_xpath`. An XPath, when nonempty, takes precedence over the CSS
selector. The raw driver normally finishes the conversation on failure;
`--retry_login_evidence true` (used by `qute-assist.exp --one-shot`) returns exit code 3
and retains the conversation only when the initial capture lacks login
evidence and no browser action has started. The exported
`authentication-verified: false` line is unchanged:
it records that no HTTP/session authentication was verified, regardless of
which evidence control matched.

## Offline run and installed use

```sh
build/default/src/cli/prowsetk run booking-admin-api \
  --config examples/booking-dotcom-admin-api/Prowse.toml \
  --html '<script>fetch("/api/reservations?limit=2")</script><form action="/api/reservations" method="post"><input name="rooms" type="number" required></form>' \
  --output _scraped/offline.yaml --postman _scraped/offline.postman.json
```

Offline runs need no broker/browser, ignore action policies, never look up
credentials or make requests, and report `offline: true` with no login evidence.
Files are committed atomically as owner-only outputs. Paths in driver arguments
are relative to the caller's working directory; module paths resolve relative
to the script. `--ipc_module /absolute/lquteipc.so` selects a nonstandard or
installed native module instead of build-preset discovery. The installed
project is at `share/prowsetk/examples/booking-dotcom-admin-api/Prowse.toml`.
