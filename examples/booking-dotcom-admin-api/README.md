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
