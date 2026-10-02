# Pagewatch

`pgwatchd` supervises independently deployed Lua DOM watchers. Each worker owns
a Flatworm browser/session, reloads a page on a configured interval, evaluates a
**core PDQL query** against the resulting DOM, and notifies the daemon over
framed Unix-socket IPC when the serialized projection changes. The daemon keeps
the latest data and a bounded update queue and can invoke a configured Lua
action. `pgwatchctl` deploys scripts and controls and inspects watchers.

The tools build on Linux when Lua and tomlplusplus are available. Modules are
embedded; no Lua package installation or graphical browser is required.

```sh
# /var/run/pagewatch is the default. It must be owned by the daemon account.
# An override is convenient for an unprivileged local run.
build/default/tools/pagewatch/pgwatchd --directory "$PWD/build/pagewatch"

build/default/tools/pagewatch/pgwatchctl --directory "$PWD/build/pagewatch" \
  deploy headings tools/pagewatch/examples/headings.lua \
  --config tools/pagewatch/examples/Watcher.toml

build/default/tools/pagewatch/pgwatchctl --directory "$PWD/build/pagewatch" status headings
build/default/tools/pagewatch/pgwatchctl --directory "$PWD/build/pagewatch" data headings
build/default/tools/pagewatch/pgwatchctl --directory "$PWD/build/pagewatch" updates headings --since 1
```

## Watcher scripts and TOML

Each deployment copies its script and normalized TOML under
`/var/run/pagewatch/watchers/NAME/` (or the override), alongside `tab.json` and
`state.toml`. The script defines `main(args)`, registers exactly one watcher,
and returns `0`:

```lua
local pgwatch = require('lpgwatch')
function main(args)
    pgwatch.watch {
        session = session, -- managed session supplied by the worker host
        query = args.query,
        before_sample = function(active)
            -- Optional trusted DOM interactions before every query.
        end
    }
    return 0
end
```

`watcher:sample()` returns `(serialized_json, changed)`. It evaluates the
installed DOM, including page JavaScript and Lua/C++ mutations. The host
refreshes the document before periodic samples. This is polling, not a live
connection to a graphical browser: transient changes between samples are not
guaranteed to be observed. Changes outside the projection do not notify the
daemon. Result equality includes values, row count and row order.

```toml
[watcher]
url = "https://example.com/"
query = '''select text, attr("data-price") from xpath("//main//*[@class='price']")'''
interval_ms = 30000
worker_timeout_ms = 60000
# html_file = "page.html" # offline source, re-read on every sample

[engine]
javascript = true
timeout_ms = 10000

[arguments]
# Extra scalar values passed to main(args).
```

`html_file` resolves relative to the deployment configuration at the controller;
offline sessions make no script/resource requests. Live traffic (including
redirects and scripts) stays same-origin through Session and NetworkClient,
with 128 transport requests per iteration. Failed HTTP requests or queries
preserve the last successful snapshot and increment `errors`. Resume a failed
or timed-out worker explicitly.

The currently supported PDQL slice includes angle-delimited tag globs,
`xpath("...")` through the core XPath engine, `select text, tag, attr("name")
from ...`, `from ... select { key: node.text }`, equality guards, trimming,
numeric conversion and count/sum/avg over the selection. Unsupported expressions
fail explicitly. This implementation does not claim the full proposed PDQL
marionette/RE2 language. Queries are limited to 64 KiB, 10,000 rows and 16 MiB
of projected/serialized data; watcher notifications additionally have a 256 KiB
bound. Default PDQL redaction removes scripts/styles, private form-control
values, sensitive attributes and sensitive URL parameters before selection.

## Commands

| Command | Effect |
|---|---|
| `ping`, `list` | Daemon readiness / watcher statuses |
| `deploy NAME SCRIPT --config FILE` | Copy and start a new named watcher |
| `update NAME SCRIPT --config FILE` | Replace a deployment and restart its worker |
| `status NAME` | State, PID, sequence, changes, errors and action counters |
| `data NAME` | Latest sequence and serialized PDQL JSON in the `data` string |
| `updates NAME --since N` | Bounded events after sequence N |
| `check NAME` | Request an immediate sample from an active worker |
| `pause NAME`, `resume NAME` | Stop / start worker execution |
| `remove NAME` | Stop execution and delete that deployment's runtime files |
| `shutdown` | Stop daemon and supervised children |

Responses are JSON; `data` is a JSON-encoded **string containing the PDQL JSON**.
Clients may parse it to get the result rows. An initial baseline advances the
sequence but does not count as a change or invoke actions unless `emit_initial`
is enabled. Updates contain `sequence`, `initial`, and `data`; responses also
include `last_sequence`, `more`, and `history_lost`. On lost history, retrieve the
latest snapshot rather than assuming uninterrupted delivery.

## Daemon actions and bounds

`pgwatchd --config Pagewatch.toml` reads:

```toml
[pagewatch]
directory = "/var/run/pagewatch"
action_script = "examples/on_change.lua"
action_timeout_ms = 5000
emit_initial = false
```

Actions define `main(args)` and return `0`. They receive `name`, `data`,
`sequence`, and `initial`, and run with the watcher directory as cwd. A separate
process with a deadline executes each action; output and detailed errors are
suppressed. Actions are serialized per watcher, with at most four running and
32 pending globally. Queue overflow increments `action_dropped`; failures and
timeouts increment `action_failures`. Scripts are trusted automation and may
explicitly use host resources.

There are at most 32 watchers, 64 control clients, 128 KiB per script/config,
1 MiB per IPC frame, and 16 retained events / 512 KiB of event payload per
watcher. Partial clients expire after two seconds. The nonblocking control loop
is separate from browser execution; a process watchdog also covers infinite Lua
loops and coroutines. Workers are also killed when the daemon exits abruptly.
Runtime directories are mode 0700, sockets/files mode
0600; symlink runtime directories and foreign owners are rejected. Socket peers
must have the daemon's effective UID. This is a same-user trust boundary.

Deployments, last sanitized snapshots and counters survive a daemon restart
within the runtime directory's lifetime. They recover **paused**. Event queues,
pending actions, browser sessions, credentials and Lua runtime state are
process-local. Snapshot and counter files are separately atomically replaced,
not a transactional database. `/var/run` is commonly tmpfs, so deployments do
not claim to survive reboot; use a persistent override when needed.

`pgwatch.service` is an installation example using a pre-provisioned `pagewatch`
account and an owner-only systemd RuntimeDirectory. Install the binaries and
configure `/etc/pagewatch/Pagewatch.toml` to match the host.
