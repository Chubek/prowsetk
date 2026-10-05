# Booking.com admin API discovery with Qutebrowser

This `Prowse.toml` project uses the
[Qutebrowser userscripts](../../tools/qutebrowser-bridge/README.md) as its assistant
browser. Log in interactively in Qutebrowser, send the current admin DOM to Lua,
and generate redacted OpenAPI 3.1 and Postman 2.1 specifications with the real
scrape-endpoints and schema-grabber plugins. Optional trusted actions turn the
same driver into a two-way marionette.

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

Live exports require an actual DOM account/logout control matching
`success_selector`, defaulting to
`a[href*='logout'], [data-testid='account-menu']`. Adjust this for your account's
current UI, or use `--success_xpath "//h1[contains(normalize-space(.), 'Your property')]"`.
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
paths. Schema enrichment uses the final capture's forms/scripts; endpoints
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
   generic `Booking assistant snapshot was not confirmed` error and preserves
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
# Qutebrowser one-shot: starts the broker, prints the one :spawn line to run
# in the logged-in tab, then runs the driver until it consumes the snapshot.
examples/booking-dotcom-admin-api/qute-assist.exp \
  --directory build/qute-booking --timeout 300

# Cloudflare snapshot-to-specs: prompts for missing credentials only, fetches
# the rendered snapshot, then runs the driver on it (needs nothing else).
CLOUDFLARE_ACCOUNT_ID=... CLOUDFLARE_API_TOKEN=... \
  examples/booking-dotcom-admin-api/browser-run-assist.exp \
  --output _scraped/booking-admin-browser-run.html
```

- `qute-assist.exp` wraps the one-shot workflow (`--actions-file` switches the
  printed line and driver to the two-way `ptk-qute-marionette` round-trip;
  `--launch` relays the broker's own `[y/N]` approval to your keystroke
  instead of answering it). Exit is the driver's exit: `0` on exported specs,
  `1` on bridge/driver failure with previous exports preserved.
- `browser-run-assist.exp` wraps `fetch-browser-run-snapshot.sh` and chains
  the result: `--to driver` (default, offline `--html` import; snapshots over
  512 KiB are refused with a pointer to `--to marionette`, which takes the
  snapshot file directly but needs a running OpenCode server).
- Both scripts default every path/URL/timeout (`--help` lists overrides), pin
  the URL to `https://admin.booking.com*`, and exit `2` on usage errors.

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
