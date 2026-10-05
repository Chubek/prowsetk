# Qutebrowser assistant-browser bridge

These executable userscripts connect Qutebrowser to ProwseTk drivers through an
owner-only local Unix socket. Start them with `:spawn --userscript <script>`.
They use Qutebrowser's documented `QUTE_HTML`, `QUTE_URL`, `QUTE_TAB_INDEX` and
`QUTE_FIFO` interface; no browser extension or remote debugging port is needed.

| Script | Purpose |
|---|---|
| `ptk-qute-send` | Send one current-tab HTML snapshot to a waiting driver |
| `ptk-qute-scrape` | Send a snapshot tagged for scrape-endpoints consumption |
| `ptk-qute-marionette` | Send the initial page, accept bounded Lua actions, and return fresh snapshots |
| `ptk-qute-bridge` | Start the local broker, inspect status, or finish a conversation |

The scripts require POSIX, Python 3.9+ (standard library only), and Qutebrowser
with userscript support. The optional `lquteipc` Lua module uses the existing
Lua dependency and builds with ProwseTk on POSIX. Qutebrowser remains an explicit
external assistant; the Flatworm core has no graphical or browser dependency.

## Build and one-shot scrape

```sh
cmake --preset default
cmake --build --preset default
tools/qutebrowser-bridge/ptk-qute-bridge serve \
  --directory build/qute-booking --url https://admin.booking.com/
```

Leave the broker running. Its default lifetime is 300 seconds. In a second
terminal, start the supplied driver (paths below are relative to the repository):

```sh
build/default/src/cli/prowsetk run booking-admin-api \
  --config examples/booking-dotcom-admin-api/Prowse.toml \
  --bridge "$PWD/build/qute-booking/bridge.json"
```

Log in normally in Qutebrowser, including any human verification or MFA. From
the confirmed admin tab, run this command with **absolute paths**:

```text
:spawn --userscript /absolute/prowsetk/tools/qutebrowser-bridge/ptk-qute-scrape --bridge /absolute/prowsetk/build/qute-booking/bridge.json
```

The sender tags its snapshot `purpose: "scrape"`. The Lua companion loads the
HTML into a JavaScript-disabled Flatworm Session, calls the real
`plugins/scrape-endpoints` Lua API, then `schema-grabber`. The broker does not
pretend that those plugins have an HTTP or IPC endpoint. Any driver can consume
the same snapshot with DOM selectors, XPath, PDQL, IR emitters or other plugins.

`serve --launch` optionally opens the configured URL after terminal approval.
`--browser` is an executable path, not a shell command. Browser diagnostics are
suppressed. Existing Qutebrowser windows remain user-owned. EOF cancels approval.

## Two-way Lua marionette

Use `ptk-qute-marionette` instead of the one-shot sender:

```text
:spawn --userscript /absolute/prowsetk/tools/qutebrowser-bridge/ptk-qute-marionette --bridge /absolute/prowsetk/build/qute-booking/bridge.json --settle-ms 500
```

The userscript stays alive while Lua inspects the page and issues actions. Each
action is generated from fixed code in an owner-only temporary JavaScript file,
submitted through `jseval --quiet --world=main --file`, and followed by a **new**
`ptk-qute-send` invocation. Rereading the initial `QUTE_HTML` would return stale
data. The capture delay is a heuristic settling interval, not a load/network-idle
guarantee. No page values or action values appear in FIFO commands.

```lua
local qute = dofile('/absolute/prowsetk/tools/qutebrowser-bridge/lua/qutebrowser_bridge.lua')
local client = qute.connect{descriptor='/private/runtime/bridge.json'}
local snap = assert(client:snapshot(0, 30000))
local session, browser = qute.load_snapshot(snap)
local first = qute.scrape(session) -- plain result table with endpoints/exports

snap = client:act({type='click', selector='button[data-next]'}, 30000)
session:load_html(snap.html, snap.url)
local next_page = qute.scrape(session)
local rows = require('lpdql').rows(session, 'select tag, text from <h*>')

session:close()
client:finish()
```

Keep the returned `browser` alive while its Session is in use. The companion
API is:

| Function / method | Result |
|---|---|
| `connect{descriptor, ipc_module?}` | Client for an explicitly configured broker |
| `client:status()` | Value-free status and action counters |
| `client:snapshot(after_revision?, wait_ms?)` | Latest fresh snapshot or `nil` on a bounded wait timeout |
| `client:act(action, wait_ms?)` | Fresh snapshot with action ID and `action_status` |
| `client:finish()` | Close the conversation and stop its marionette |
| `load_snapshot(snapshot)` | JavaScript-disabled Session and its owning Browser |
| `scrape(session, {api_only?})` | scrape-endpoints result |
| `enrich(session, endpoints, {api_only?, collection_name?})` | schema-grabber result; no GET probes |
| `write(path, bytes, ipc_module?)` | Atomic `0600` file; new directories are `0700` |

Supported actions are strictly `click {selector}`, `fill {selector, value}`,
`navigate {url}` and `reload`. `fill` uses the prototype value setter plus
input/change events; it rejects password/file/hidden/button/checkable controls.
Actions check the approved origin at execution and reject hidden/disabled
targets. There is no arbitrary JavaScript, Qutebrowser command, shell, or model
output execution API. Clicks use DOM `click()`, not hardware pointer events.

`action_status` is `ok` or `error` when a DOM marker survives until capture;
navigation may remove it, producing `unconfirmed`. Neither a marker nor an
action reply proves that the site's workflow succeeded. Drivers must inspect
the fresh DOM. Actions are never automatically retried after a timeout.

## Installation and runtime paths

CMake installs scripts/helpers together under
`share/prowsetk/qutebrowser-bridge/`, Lua under its `lua/` subdirectory, the native
module under `lib/prowsetk/lua/lquteipc.so`, and the Booking project under
`share/prowsetk/examples/booking-dotcom-admin-api/`. Use absolute userscript
paths, or link the scripts into `~/.local/share/qutebrowser/userscripts/`.
For example, a binding in Qutebrowser's `config.py` can run:

```python
config.bind(',ps', 'spawn --userscript /absolute/ptk-qute-scrape --bridge /private/runtime/bridge.json')
```

`PROWSETK_QUTE_BRIDGE` provides the descriptor path when inherited by
Qutebrowser; `--bridge` takes precedence. No token is passed on the command line.
In the source tree the Lua helper searches common build presets for `lquteipc`.
For installed or nonstandard builds, pass `ipc_module` explicitly or include
the native module directory in Lua's `package.cpath`. Scripts run entirely
offline when the Booking driver's `html` argument is supplied.

```sh
tools/qutebrowser-bridge/ptk-qute-bridge status --bridge build/qute-booking/bridge.json
tools/qutebrowser-bridge/ptk-qute-bridge finish --bridge build/qute-booking/bridge.json
```

`finish` closes the conversation; the broker process remains bounded by
`--timeout`. SIGINT/SIGTERM removes its socket and descriptor. An occupied
runtime is refused; after an abrupt kill, remove stale `bridge.sock` and
`bridge.json` explicitly before starting a new broker.

## Scope, protocol and bounds

The private descriptor (`0700` directory, `0600` file/socket) contains
`{version: 1, socket, token, origin}`. Each socket connection sends one
newline-terminated JSON request containing `version`, `token`, and `op`, and
receives one JSON reply with `ok`. Operations are `publish`, `snapshot`,
`attach`, `next`, `act`, `detach`, `status`, and `finish`. Duplicate keys, unknown
fields/actions, excessive nesting and unauthenticated requests are rejected.
Linux also checks the peer UID. Same-user processes are trusted and can read
the token; this is not isolation from other programs owned by that user.

Only the latest snapshot is retained in memory. HTML is transferred verbatim
and may contain private page data; it is never printed or persisted by the
broker. Generated specifications use the plugins' default redaction, provenance
and confidence, and explicitly report incomplete heuristic coverage.
Schema enrichment removes all form values from a detached snapshot, preserving
field names/types/required flags without changing the caller's document.

| Bound | Value |
|---|---|
| Broker lifetime | 1–3600 seconds; default 300 |
| Simultaneous clients / active marionettes | 8 / 1 |
| HTML / encoded message | 4 MiB / 8 MiB (both bounds apply) |
| JSON nesting / total actions | 32 / 0–32, default 32 |
| Action selector / fill value | 4096 UTF-8 bytes each |
| IPC wait / partial-client timeout | 0–30 seconds / 5 seconds |
| Settling interval | 0–5000 ms; default 500 |
| Private output file | 16 MiB |

The first capture/attachment pins the Qutebrowser **tab index**. Keep that tab
active in the same window, and do not reorder/close tabs during a marionette.
The userscript API has no stable tab ID: it cannot distinguish a replacement
same-origin tab at the same index. Captures from another origin/index fail;
in-page actions also guard the origin. Browser-driven redirects and page
requests obey Qutebrowser's policies, not ProwseTk's host transport policy.

Userscripts expose rendered DOM snapshots, not cookies, HttpOnly credentials,
historical network requests, HTTP response status or response bodies. The
bridge does not provide full HAR, remote module execution, cookie export, or
automatic challenge clearance. It does not import authentication into the
Flatworm Session. GET response probing is disabled; request schemas are
inferred from captured forms/scripts and URL parameters. See the
[Booking project](../../examples/booking-dotcom-admin-api/README.md) for the
end-to-end workflow and its DOM login-evidence check.

Tests are registered under `tests/unit/` and `tests/integration/`, including
real Lua/CLI/plugin composition with a simulated Qutebrowser FIFO. They require
no display or public network:

```sh
ctest --preset default -R 'qutebrowser|Qutebrowser'
```
