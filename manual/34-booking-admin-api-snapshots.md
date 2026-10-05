# Chapter 34: Booking Admin API Snapshots

[Manual index](README.md)

## The shipped project

`examples/booking-dotcom-admin-api/Prowse.toml` registers the `booking-admin-api`
driver in `api.lua`. It consumes the admin page captured by Qutebrowser,
optionally requests a trusted sequence of actions, and composes scrape-endpoints
with schema-grabber to produce OpenAPI 3.1 YAML and Postman 2.1 JSON.

The workflow has two sources: a live assistant snapshot and an offline `html`
argument. Both load JavaScript-disabled Flatworm Sessions. Interactive login,
MFA, and human verification happen in the user-owned Qutebrowser tab. The driver
does not accept login credentials or transfer browser cookies.

```text
Initial HTML → confirm live DOM evidence → scrape endpoints
  → optional action + fresh capture + confirm evidence + scrape, repeated
  → bounded same-origin endpoint merge
  → sanitized snapshot schema enrichment
  → redacted OpenAPI and Postman with incomplete-coverage metadata
```

Chapters 31–33 explain installation, the Lua API, and action semantics. This
chapter is the end-to-end project recipe.

## Begin with a deterministic offline run

From the repository root:

```sh
build/default/src/cli/prowsetk run booking-admin-api \
  --config examples/booking-dotcom-admin-api/Prowse.toml \
  --ipc_module "$PWD/build/default/tools/qutebrowser-bridge/lquteipc.so" \
  --html '<script>fetch("/api/reservations/123?limit=2")</script><form action="/api/reservations" method="post"><input name="rooms" type="number" required value="2"></form>' \
  --output _scraped/booking-offline.yaml \
  --postman _scraped/booking-offline.postman.json
```

Build with the Chapter 31 commands first. Offline mode uses the supplied HTML,
skips live login evidence and action-file loading, and makes no page requests.
It requires neither a running broker nor Qutebrowser. Native IPC/file-I/O support
is still used to commit owner-only outputs.

The example supplies GET and POST evidence, a numeric URL segment/query value,
and a required numeric form field. The exports retain typed parameters and
request properties, remove form value examples, and report offline/incomplete
metadata. Lua's internal schema records retain the form-field `required` flag,
but its current OpenAPI renderer does not emit request-object `required`
arrays. The script's fetch call is inspected as source; it is not executed.

## Live one-shot export

Start a private broker for the admin origin:

```sh
tools/qutebrowser-bridge/ptk-qute-bridge serve \
  --directory build/qute-booking --url https://admin.booking.com/ \
  --timeout 300
```

Open Qutebrowser, log in, and reach the account's admin UI. `serve --launch`
can open the configured URL after terminal approval. Then start the receiver
in another terminal:

```sh
build/default/src/cli/prowsetk run booking-admin-api \
  --config examples/booking-dotcom-admin-api/Prowse.toml \
  --bridge "$PWD/build/qute-booking/bridge.json" \
  --ipc_module "$PWD/build/default/tools/qutebrowser-bridge/lquteipc.so" \
  --output _scraped/booking-admin-openapi.yaml \
  --postman _scraped/booking-admin-postman.json
```

With the admin tab active, send the page using actual absolute paths:

```text
:spawn --userscript /absolute/prowsetk/tools/qutebrowser-bridge/ptk-qute-scrape --bridge /absolute/prowsetk/build/qute-booking/bridge.json
```

The driver waits up to `wait_ms` for a snapshot; the default is 30 seconds.
A capture can be published first and consumed when the driver starts. Every
live run requires both the configured URL and broker origin to be
`https://admin.booking.com`.

The project's `[assistant-browser]` table declares Qutebrowser and the userscript
method. Broker startup and sender invocation are the explicit steps above;
the driver does not automatically launch them. Disabling the injected
assistant-browser setting rejects live operation. Offline runs remain usable.

## Configure positive DOM evidence

The default `success_selector` is:

```css
a[href*='logout'], [data-testid='account-menu']
```

The driver requires a matching DOM element on the initial live capture and
after every action. Script/style/meta nodes are excluded from evidence. A
word in JavaScript source is not a logout control. The default selector is a
starting point; adjust it to an authenticated-only control in the actual UI.

Alternatively pass an XPath expression:

```sh
build/default/src/cli/prowsetk run booking-admin-api \
  --config examples/booking-dotcom-admin-api/Prowse.toml \
  --bridge "$PWD/build/qute-booking/bridge.json" \
  --success_xpath "//h1[contains(normalize-space(.), 'Your property')]"
```

A nonempty `success_xpath` takes precedence over `success_selector`. This
example is a placeholder for a property-specific beacon, not a universal
Booking.com selector. Presence is the current evidence test; it does not
verify a browser-rendered visibility condition or a successful HTTP response.

The exported distinction is intentional: `login-evidence: true` records the
DOM match, while `authentication-verified: false` records that no HTTP/session
authentication was verified. Qutebrowser's userscript data supplies no status,
cookie jar, or response bodies.

## Two-way account navigation

Copy `examples/booking-dotcom-admin-api/actions.example.json` to a local policy
and use selectors for the actual account UI. For example:

```json
{
  "version": 1,
  "actions": [
    {"type": "click", "selector": "button[role='tab'][aria-controls='reservations']"}
  ]
}
```

Start a fresh broker, add `--actions_file /absolute/my-actions.json` to the
receiver command, and connect the tab with:

```text
:spawn --userscript /absolute/prowsetk/tools/qutebrowser-bridge/ptk-qute-marionette --bridge /absolute/prowsetk/build/qute-booking/bridge.json --settle-ms 500
```

The policy is trusted, bounded to 256 KiB and 32 actions, and subject to the
broker's action budget. Actions are fixed click/fill/navigate/reload operations
from Chapter 33. The example selector is illustrative.

The driver collects endpoints before and after each action, deduplicating by
method/URL and rejecting seeds outside the admin origin. It retains at most
10,000 seeds. An action reply with status `error` aborts the run; an
`unconfirmed` reply is accepted only if its fresh page still passes the DOM
evidence check. The login beacon does not verify every action's intended effect.

## Arguments and paths

| Argument | Type / default | Meaning |
|---|---|---|
| `url` | URL / `https://admin.booking.com/` | Snapshot base URL; live origin fixed to admin |
| `html` | optional string | Select offline mode, including for an empty string |
| `bridge` | optional path | Descriptor; otherwise `PROWSETK_QUTE_BRIDGE` |
| `ipc_module` | optional path | Native module override; otherwise helper discovery |
| `wait_ms` | integer / 30000 | 1–30,000-ms snapshot/action wait |
| `success_selector` | string / logout/account selector above | Live DOM evidence |
| `success_xpath` | optional string | Override evidence with XPath |
| `actions_file` | optional path | Trusted live action policy; ignored offline |
| `api_only` | boolean / true | Keep proper API-like endpoints |
| `output` | path / `_scraped/booking-admin-openapi.yaml` | OpenAPI output |
| `postman` | path / `_scraped/booking-admin-postman.json` | Postman output |

Driver argument paths resolve relative to the caller's working directory.
The script/helper/plugin lookup uses source- or install-relative locations.
Use `--api_only false` to retain a broader same-origin surface, or an empty
output argument to omit that artifact. Offline mode can use another HTTP(S)
base URL; live mode requires the admin origin.

The installed configuration is
`share/prowsetk/examples/booking-dotcom-admin-api/Prowse.toml`. Use the installed
`prowsetk` and pass the installation's `lib/prowsetk/lua/lquteipc.so` when it is
outside `package.cpath`. `lib` may differ for a customized installation.

## Schemas, redaction, and coverage

API-only filtering defaults on and excludes ordinary pages/static assets.
Discovery uses captured DOM and inline script references, with method,
provenance, and confidence. There is no automatic link crawl, recursive GET
resolution, SPA network probe, or response-schema probe in this workflow.

Schema-grabber's Lua path infers typed URL parameters/path templates and request
fields from matching forms in the final snapshot. Form values are removed in
a detached copy; the original snapshot remains queryable. Endpoints found on
earlier captures are retained, but form fields available only on those earlier
documents are not accumulated as observed request schemas.

Responses without bytes retain heuristic defaults. Captured script references
are not observed calls. All exports preserve redaction and inference metadata;
coverage stays explicitly incomplete.

An illustrative successful live export with one marker-confirmed action has:

```yaml
x-prowsetk-qutebrowser:
  used: true
  offline: false
  login-evidence: true
  authentication-verified: false
  complete: false
  snapshots: 2
  actions-dispatched: 1
  actions-confirmed: 1
  source: 'Assistant DOM and inline scripts; heuristic discovery.'
  response-probes: false
```

Postman carries the same metadata block using underscore field names such as
`login_evidence`, `authentication_verified`, and `actions_confirmed`.
The counters measure snapshots and returned action/marker results; confirmed
markers are not proof of application success. Offline runs set `used: false`,
`offline: true`, and `login-evidence: false`.

Each output is atomically replaced as a `0600` file; newly created directories
use `0700`. OpenAPI and Postman commits are independent. A failure during the
second write can leave a new first artifact, so consumers should check the
driver exit code as well as the files. Login/parse/action failure before export
preserves previous artifacts. The driver closes its Session and attempts to
finish the conversation during cleanup; errors are generic and value-free.

## Verification and further work

The hermetic suites exercise actual Lua/plugin composition, offline CLI
arguments, live DOM evidence, parse rejection, owner-only output, redaction,
origin/tab scope, and a simulated FIFO action/capture loop:

```sh
ctest --preset default -R 'qutebrowser|Qutebrowser'
ctest --preset asan -R 'qutebrowser|Qutebrowser'
```

Neither command needs a live Booking account or desktop. To add custom DOM
postconditions or other projections, build a driver around the companion API
from Chapters 32–33. Chapter 24 documents the separate crawler's authenticated
HTTP/cookie workflow and its host-enforced request budgets.

Reference: [project notes](../examples/booking-dotcom-admin-api/README.md),
`examples/booking-dotcom-admin-api/Prowse.toml`, `api.lua`,
`tests/integration/test_drivers.cpp`, `tests/integration/test_cli.cpp`.

**Next:** [CDP client and remote control](35-cdp-client-and-remote-control.md).
