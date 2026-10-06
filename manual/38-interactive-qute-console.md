# Chapter 38: Interactive Qutebrowser Lua Console

[Manual index](README.md)

## What this host is

`ptk-qute-repl` is a persistent terminal host for one bridge conversation. It
owns a `LuaRuntime`, keeps one managed snapshot Session alive, and lets you
inspect the DOM, issue the fixed browser orders from Chapter 33, and write your
own Lua against `session`, `document`, and `qute`.

```text
you                  ptk-qute-repl                  bridge               Qutebrowser
 │   :targets  ──────▶ ordered inspection list
 │   :click 4  ──────▶ client:act ──────────────────▶ jseval ────────────▶ page changes
 │                    ◀──── fresh capture + schema ───  ◀──── capture ─────  ptk-qute-send
 │   :export  ──────▶ merged OpenAPI + Postman
```

It is a *host*, not a sandbox. Its input is trusted automation typed by the
operator, evaluated synchronously in the ProwseTk Lua runtime. It is not a place
to run page or model output, and it never executes a captured script. Every
browser effect goes through the same fixed, validated action channel documented
in Chapter 33, and every page byte arrives through the DOM snapshot boundary of
Chapters 31 and 39.

Chapter 34 remains the end-to-end Booking.com recipe; this chapter documents
the console itself, which works with any approved origin.

## Build and locate

The console is optional and off the critical path. It builds when POSIX, Lua,
and Replxx are present, and it can be removed without affecting the core, the
userscripts, or the finite driver. Build options are listed in
[Chapter 2](02-build-and-installation.md):

```sh
cmake --preset default
cmake --build --preset default
```

`-DPROWSETK_BUILD_QUTE_REPL=OFF` omits the executable. A missing Replxx reports
`Replxx not found: ptk-qute-repl is unavailable` and skips the target; the
bridge scripts, `lquteipc`, and `booking-admin-api` still build. Discovery lives
in `cmake/Dependencies.cmake`, which prefers an installed `replxx` CMake package
and otherwise adds the unmodified `third_party/replxx` source tree.

| Option | Default | Meaning |
|---|---|---|
| `PROWSETK_BUILD_QUTE_REPL` | `ON` | Build `ptk-qute-repl` when Lua and Replxx are available |

The executable finds its own helpers. `--root` selects the directory holding
`lua/console.lua` and the Lua companion; `--ipc-module` selects `lquteipc.so`.
Without them, the host tries, in order, its install prefix
(`share/prowsetk/qutebrowser-bridge`, `lib/prowsetk/lua/lquteipc.so`), the
compiled-in installation paths, and finally the source and preset build trees.
Keep the console and its module from the same build; mixing a sanitized host
with an uninstrumented module changes what its leak checks actually cover.

## Start a session

Start the broker as in Chapter 31, then attach the marionette from your
authenticated tab:

```text
:spawn --userscript /absolute/prowsetk/tools/qutebrowser-bridge/ptk-qute-marionette --bridge /absolute/prowsetk/build/qute-demo/bridge.json --settle-ms 500
```

Launch the console:

```sh
tools/qutebrowser-bridge/ptk-qute-repl --bridge build/qute-demo/bridge.json \
  --require-login --output build/qute-demo/openapi.yaml \
  --postman build/qute-demo/postman.json
```

`examples/booking-dotcom-admin-api/qute-assist.exp` wraps this sequence: it
starts the broker, prints the one `:spawn` line, and hands the terminal to the
console. `--repl-bin` selects another preset's executable. The interactive
launcher defaults to a 1,800-second broker lifetime and a 256-action budget;
`--timeout` accepts 1–3600 seconds and `--max-actions` accepts 0–256. Because
the console is optional, the launcher checks for the executable first and
reports where to obtain it instead of failing later; `--one-shot` selects the
finite driver, which needs no Replxx.

On startup the console connects, performs one non-blocking snapshot check, and
prints a ready line. If the broker already retained a capture, that page is
installed immediately; otherwise it tells you to attach the marionette and use
`:capture`.

| `ptk-qute-repl` option | Default | Meaning |
|---|---|---|
| `--bridge` | required | Private `bridge.json` descriptor |
| `--output` / `--postman` | `_scraped/...` defaults | `:export` destinations |
| `--require-login` | off | Require positive DOM evidence for discovery and export |
| `--success-selector` / `--success-xpath` | none | Explicit login beacon overrides |
| `--wait-ms` | 30000 | Capture/action wait, 1–30,000 |
| `--ipc-module` / `--root` | discovered | Native module and helper directory |
| `--show-values` | off | Display target labels in this local console |
| `--include-noise` | off | Retain telemetry and error-reporting candidates |
| `--batch` | off | Read orders and Lua from standard input |
| `--script` | none | Run a trusted host Lua file, then close |

## Orders

Orders start with `:` and are parsed, never evaluated. `:help` prints the same
list with the Lua examples.

| Order | Effect |
|---|---|
| `:capture` | Fresh DOM from the attached marionette, or wait for a new revision |
| `:status` | Revision, capture, endpoint, evidence, and broker counters |
| `:targets [SELECTOR]` | Numbered structural targets for the current capture |
| `:links` / `:forms` | `:targets` restricted to `a[href]` or form controls |
| `:click TARGET` | Chapter 33 click order |
| `:fill NUMBER "value"` | Set a text control through its native setter |
| `:select NUMBER "value"` | Choose an existing option and emit input/change |
| `:check NUMBER true\|false` | Drive a checkbox or radio to that state |
| `:focus NUMBER` / `:scroll NUMBER` | Focus or scroll an interactable target into view |
| `:submit NUMBER` | `requestSubmit` on a same-origin form |
| `:navigate /relative/path` | Same-origin navigation resolved against the current page |
| `:reload` | Reload the tab |
| `:discover` | Re-run discovery and enrichment on the current capture |
| `:endpoints` | List accumulated operations |
| `:export` | Write the accumulated OpenAPI and Postman specifications |
| `:values on\|off` | Toggle local target labels |
| `:help` / `:quit` | Help, or close the conversation |
| `:cancel` | Discard pending multiline input |

`TARGET` is either a number from the most recent inspection list or a literal
CSS selector. `:fill`, `:select`, and `:check` take a number plus a value;
the value is read as a Lua expression, so strings need their own quotes.

Every order that touches the browser returns a fresh capture. After any
successful capture, any rejected capture, or any lost action reply, the numbered
target list and the login evidence are invalidated together — the console will
not act on a target list that may describe an older DOM. Re-run `:targets`
after any capture.

## The Lua surface

Anything not prefixed with `:` is Lua, evaluated in the same persistent runtime,
so globals and functions persist between inputs:

```lua
retained = 7
function total(n) return n + retained end
assert(total(3) == 10)

qute:click("button[data-next]")
qute:fill("input[name='date']", "2027-01-01")
qute:beacon("[data-testid='account-menu']")
document:query_selector_all("form")
require("lpdql").rows(session, "select tag, text from <h*>")
for _, endpoint in ipairs(qute.endpoints) do print(endpoint.method, endpoint.path) end
```

An input is first tried as a `return` expression, then as a statement chunk. If
the chunk is incomplete, the console shows a ` ...> ` continuation prompt and
keeps reading until the chunk parses; `:cancel` discards it. Completion, inline
hints, and history come from Replxx. History lives in memory only and is cleared
when the process exits, so nothing typed here is written to disk.

Three globals are published and refreshed on every capture:

| Global | Meaning |
|---|---|
| `qute` | The console controller, documented below |
| `session` | Current managed snapshot Session |
| `document` | `session:document()` for the current capture |

`session` is JavaScript-disabled, redirects are not followed, and network
observation is off, exactly as in Chapter 32. A new capture replaces the
document, so reacquire Document and Element handles after every order.

| `qute` member | Contract |
|---|---|
| `qute:targets(selector?)`, `qute:links()`, `qute:forms()` | Inspection rows; also repopulate numbered targets |
| `qute:click/focus/scroll/submit(target)` | Element orders |
| `qute:fill/select(target, value)`, `qute:check(target, checked)` | Value orders |
| `qute:navigate(url)`, `qute:reload()`, `qute:capture()` | Navigation and capture orders |
| `qute:beacon(css, xpath)` | Set a login beacon; returns the evidence result |
| `qute:discover()` | Re-run discovery and enrichment |
| `qute:endpoints` | Accumulated endpoint tables |
| `qute:schemas` | Accumulated sanitized schema records |
| `qute:status()`, `qute:list_endpoints()` | Value-free counters and operation list |
| `qute:export(output?, postman?)` | Serialize accumulated evidence and write both artifacts |
| `qute:show_values(bool)` | Toggle local label display |
| `qute.close()` | Close the Session and finish the conversation |
| `qute.current`, `qute.confirmed`, `qute.login_evidence` | Current-capture and evidence state |
| `qute.revision`, `qute.snapshots`, `qute.session`, `qute.browser` | Capture counters and managed handles |

`:targets` returns structural selectors built from tag names and
`nth-of-type` positions, plus the tag, capture revision, and — for links and
forms — the absolute path with its query removed. It is a navigation aid for
choosing a target, not a rendering of the page.

## State, discovery, and export

Discovery runs automatically on every confirmed capture. It keeps the
snapshot's forms and inline scripts as evidence, enriches them before the page
is replaced, and merges the result into the accumulated set keyed by method and
templated path, as Chapter 20 describes. Later captures therefore contribute
query names and form fields to an operation already known, and an export holds
one entry per operation rather than one per capture.

`:export` refuses to run without a current capture, without confirmed login
evidence when `--require-login` is set, or with nothing accumulated — an empty
result cannot overwrite useful artifacts. It writes the redacted OpenAPI YAML
and Postman JSON atomically and appends an `x-prowsetk-qutebrowser` block
recording interactive use, capture count, login evidence, and that
authentication, response probing, and completeness were not established:

```yaml
x-prowsetk-qutebrowser:
  interactive: true
  snapshots: 3
  login-evidence: true
  authentication-verified: false
  complete: false
  response-probes: false
```

Exports remain heuristic. A DOM capture contains no external bundles, no
historical network trace, and no response bytes, so response schemas are never
observed through this interface.

## Values, trust, and errors

Built-in orders are structural by default: `:targets`, `:links`, `:forms`, and
`:endpoints` omit control labels and form values unless `:values on` or
`--show-values` is set. Two explicit exceptions remain under your control:
entering Lua, and `:fill`/`:select` values, which necessarily carry the value
you type.

Errors are reported by stage and carry a fixed hint instead of the source or
the raw exception:

```text
qute: snapshot parsing failed: check the 16 MiB, 250000-node and 256-level bounds
qute: browser action failed: attach ptk-qute-marionette; use a current target; inspect the fresh DOM after failure
```

The stages are bridge connection, snapshot receipt, snapshot parsing, login
beacon validation, endpoint discovery, schema enrichment, browser action, bridge
status, DOM inspection, export validation, schema serialization, and the two
export writes. Page values, Lua source, and exception text are not part of a
console diagnostic.

Host Lua is trusted synchronous automation. There is no asynchronous
cancellation guarantee inside a long loop, and a loop can block the terminal
indefinitely. Page data and model output are never executed here; only what you
type is evaluated.

## Bounds and exit codes

| Bound | Value |
|---|---|
| Installed captures | 2,048 |
| Accumulated operations | 10,000 |
| Rows per inspection or operation list | 200 |
| Input line / assembled multiline chunk | 64 KiB / 256 KiB |
| History entries, hint rows, hint delay | 200, 1, 250 ms |
| Bulk transfer per hop | 10 seconds |

| Exit | Condition |
|---|---|
| 0 | Orderly interactive close; or every batch/scripted input succeeded |
| 1 | Broker connection or initialization failed; `--script` failed; a `--batch` input failed |
| 2 | Invalid options, or helpers could not be located |

An interactive session exits 0 on `:quit` or end of input regardless of earlier
individual failures, so a failing order is visible in its message rather than
only in the status. Use `--batch` when a script needs the exit code.

## Non-interactive use

`--batch` reads the same orders and Lua from standard input, which is useful for
a recorded exploration or a checked-in script:

```sh
printf ':capture\n:endpoints\n:export\n:quit\n' |
  tools/qutebrowser-bridge/ptk-qute-repl --bridge build/qute-demo/bridge.json \
    --require-login --batch
```

`--script FILE` runs a trusted Lua file against the live conversation and then
exits, so one `--script` invocation can perform capture, inspection, orders,
and export in a single process. Neither mode needs a terminal, and neither
performs work that the interactive path cannot.

## Verification

The console, its Lua state model, the FIFO transport it uses, and the launcher
are covered by hermetic suites with no desktop and no public network:

```sh
ctest --preset default -R 'qutebrowser|Qutebrowser'
ctest --preset asan -R 'qutebrowser|Qutebrowser'
```

`tests/integration/test_qutebrowser_repl.py` drives the real executable for
multi-megabyte captures over both FIFO hops, a multi-page action sequence with
persistent Lua, rejected-capture recovery, refused empty exports, a Replxx
terminal through a pseudo-terminal, and the Expect launcher end to end.

Reference: `tools/qutebrowser-bridge/repl.cpp`,
`tools/qutebrowser-bridge/lua/console.lua`,
`tools/qutebrowser-bridge/lua/evidence.lua`,
`tests/integration/test_qutebrowser_repl.py`.

**Previous:** [OAuth assistance](37-oauth-assistance.md).
**Next:** [Snapshot bulk transport](39-snapshot-bulk-transport.md).
