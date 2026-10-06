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
| `ptk-qute-repl` | Persistent Replxx/Lua terminal for inspection, browser orders and accumulated exports |

The scripts require POSIX, Python 3.9+ (standard library only), and Qutebrowser
with userscript support. The optional `lquteipc` Lua module uses the existing
Lua dependency and builds with ProwseTk on POSIX. Qutebrowser remains an explicit
external assistant; the Flatworm core has no graphical or browser dependency.

## Interactive Lua session

```sh
cmake --preset default
cmake --build --preset default
examples/booking-dotcom-admin-api/qute-assist.exp --directory build/qute-booking
```

The launcher starts the broker and `ptk-qute-repl`, and prints one
`ptk-qute-marionette` userscript command to run in your already-logged-in tab.
Use `:capture`, `:targets`, `:links`, `:forms`, `:click NUMBER`, `:fill NUMBER
"value"`, `:select NUMBER "value"`, `:check NUMBER true`, `:focus`, `:scroll`,
`:submit`, `:navigate /path`, `:reload`, `:discover`, `:endpoints`, `:export`,
`:status`, `:help`, and `:quit`. Target numbers belong to the last inspection
list and are invalidated by every capture. Orders return a fresh DOM; inspect it
to confirm workflow success. Capture waits and action counts remain bounded.
A rejected capture or lost action reply also invalidates targets and login
evidence; obtain a fresh valid capture before discovery or export.

The console supports editing, tab completion, hints, multiline Lua (continuation
prompt), `:cancel` and in-memory history. State is persistent; use Lua globals
to retain values between separately evaluated chunks. No history file is saved.
The globals `qute`, `session` and `document` expose the controller and current
managed snapshot. For example:

```lua
qute:click("button[data-next]")
qute:fill("input[name='date']", "2027-01-01")
qute:beacon("[data-testid='account-menu']")
require('lpdql').rows(session, 'select tag, text from <h*>')
for _, ep in ipairs(qute.endpoints) do print(ep.method, ep.path) end
```

Terminal orders omit page labels and form values by default; `:values on` or
`--show-values` opts into local target labels. Explicit Lua queries/prints can
display private values. Host Lua is trusted automation, not a sandbox; loops
and scripts run synchronously, with no asynchronous cancellation guarantee.
Page/model output is never executed. Errors omit source and raw exceptions.

Every confirmed page contributes same-origin discoveries and sanitized schemas
from scrape-endpoints/schema-grabber. Repeated methods and templated paths merge
query names and form fields across captures into a single operation, with an
evidence count and conservatively widened scalar types. It drops error-reporting/telemetry paths
(including `/js_errors`), and refuses empty exports so noise-only captures do
not overwrite useful files. `--include-noise` restores those candidates.
Schema inference remains heuristic, and no response requests are issued.
Anchor `href` values are harvested from the loaded snapshot without any
request, so an API-shaped link contributes a GET endpoint with `html-link`
provenance; link following, recursive resolution and SPA probing stay disabled.
A link is something the page offers, not an observed application call, so
capture the sections those links point at to reveal their endpoints.
Explore application sections, filters, pagination, details and lazy-load
controls; a DOM snapshot does not contain external bundles or historical HAR.
The optional captcha-handler classifies captured challenges without bypassing
them. Complete login/MFA/challenges in Qutebrowser and capture again.

For an already-running broker, run the executable directly:

```sh
build/default/tools/qutebrowser-bridge/ptk-qute-repl \
  --bridge "$PWD/build/qute-booking/bridge.json" --require-login
```

`--batch` evaluates commands/Lua from stdin; `--script FILE` runs trusted Lua
through the same controller then closes the conversation. `--root DIR` and
`--ipc-module FILE` override helper locations. The executable picks the IPC
module beside its preset and supports installed/relocatable helper paths.
`PROWSETK_BUILD_QUTE_REPL=OFF` or missing Lua/Replxx omits only this executable;
the launcher supports `--one-shot` for the finite driver below.

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
| `scrape(session, {api_only?, include_noise?, follow_links?})` | scrape-endpoints result; telemetry/noise omitted and anchors harvested by default |
| `enrich(session, endpoints, {api_only?, collection_name?})` | schema-grabber result; no GET probes |
| `write(path, bytes, ipc_module?)` | Atomic `0600` file; new directories are `0700` |

Supported actions are `click {selector}`, `fill {selector, value}`,
`navigate {url}`, `reload`, `capture`, `focus {selector}`, `scroll {selector}`,
`select {selector, value}`, `check {selector, checked}` and `submit {selector}`.
`select` requires a select element, `check` uses checkbox/radio activation,
`scroll` calls `scrollIntoView`, and `submit` uses `requestSubmit` on a same-origin
form. `fill` uses the prototype value setter plus
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
module under `lib/prowsetk/lua/lquteipc.so`, the REPL under `bin/ptk-qute-repl`, and the Booking project under
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
`{version: 1, socket, token, origin, bulk: "fifo-v1"}`. The optional bulk field
preserves legacy inline clients. Each socket connection sends one
newline-terminated JSON request containing `version`, `token`, and `op`, and
receives one JSON reply with `ok`. Operations are `publish`, `snapshot`,
`attach`, `next`, `act`, `detach`, `status`, and `finish`. Duplicate keys, unknown
fields/actions, excessive nesting and unauthenticated requests are rejected.
New senders and Lua clients stream HTML through `0600` named FIFOs in the private
runtime directory; the Unix socket exchanges only metadata/control for those
transfers. An upload carries `html_fifo` (a generated basename) and `html_bytes`;
snapshot/action requests select `transport: "fifo"`. The body frame is ASCII
`QHTML1\n`, an unsigned 64-bit big-endian byte count, and exactly that many raw
UTF-8 bytes followed by EOF. Extra/truncated data, wrong lengths/types/owners,
symlinks, unsafe paths and stalled peers fail. JSON escaping no longer inflates
the HTML body. FIFO backpressure is handled with partial nonblocking reads and
writes, not one large PIPE_BUF write. Normal exit/failure unlinks transfer FIFOs;
an abrupt process kill may require removing its leftover empty FIFO node.
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
| FIFO HTML / encoded control or legacy inline message | 16 MiB / 8 MiB |
| JSON nesting / total actions | 32 / 0–256, default 256 |
| Action selector / fill value | 4096 UTF-8 bytes each |
| IPC wait / partial-client timeout | 0–30 seconds / 5 seconds |
| Settling interval | 0–5000 ms; default 500 |
| Private output file | 16 MiB |
| Bulk FIFO transfer | 10 seconds per hop, in addition to the control wait |
| REPL captures / accumulated schemas / inspection list | 2,048 / 10,000 / 200 |
| REPL line / multiline chunk / memory history | 64 KiB / 256 KiB / 200 entries |

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
