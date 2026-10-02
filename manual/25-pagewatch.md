# Chapter 25: Pagewatch

[Manual index](README.md)

## Daemon and deployed watchers

Linux `pgwatchd` supervises independent Lua watcher workers. Each owns a
Flatworm Session, periodically reloads a live URL or offline HTML file, queries
selected DOM data through PDQL, and sends projection changes over framed IPC.
`pgwatchctl` deploys scripts and controls/inspects their data.

In one terminal start the daemon:

```sh
pgwatchd --directory "$PWD/build/pagewatch"
```

From another terminal at the repository root:

```sh
pgwatchctl --directory "$PWD/build/pagewatch" deploy headings \
  tools/pagewatch/examples/headings.lua \
  --config tools/pagewatch/examples/Watcher.toml
pgwatchctl --directory "$PWD/build/pagewatch" status headings
pgwatchctl --directory "$PWD/build/pagewatch" data headings
```

The default directory is `/var/run/pagewatch`, owned by the daemon account.
The override permits unprivileged local operation. Modules are embedded; build
requirements are Linux, Lua, and tomlplusplus, with pugixml for XPath queries.

## Watcher script and configuration

```lua
local pgwatch = require('lpgwatch')
function main(args)
    pgwatch.watch {
        session = session,
        query = args.query,
        before_sample = function(active)
            -- Optional trusted DOM actions before each projection.
        end
    }
    return 0
end
```

Register exactly one watcher and return 0. `watcher:sample()` returns serialized
JSON and a changed boolean; the worker arranges periodic refreshes. Use the
provided session. A `before_sample` callback can mutate/interact with its DOM
before querying, with the host's bounded transport policy.

```toml
[watcher]
url = "https://example.test/"
html_file = "page.html"
query = 'select text from <h1>'
interval_ms = 30000
worker_timeout_ms = 60000

[engine]
javascript = true
timeout_ms = 10000
```

This is an offline watcher: create page.html beside the config, then change
that file to exercise updates. `html_file` is resolved to an absolute path by
the controller and re-read each sample; it is not copied as the watched source.
Live mode omits that field. Offline scripts may mutate HTML without network
requests. Extra scalar `[arguments]` values are passed to main; url/query are
reserved.

Intervals are 20–86,400,000 ms, default 30,000. Worker deadlines are
100–300,000 ms, default 60,000, and must cover the live request timeout.
Engine settings share crawler's browser fields. Live requests, redirects, and
page scripts remain same-origin with 128 transport operations per iteration.

## Controller reference

| Command | Effect |
|---|---|
| `ping`, `list`, `shutdown` | Readiness, watcher listing, orderly shutdown |
| `deploy NAME SCRIPT --config FILE` | Copy script/normalized config and start watcher |
| `update NAME SCRIPT --config FILE` | Replace deployment and restart worker |
| `status NAME` | State, PID, sequence, changes/errors and action counters |
| `data NAME` | Latest sequence and serialized PDQL JSON |
| `updates NAME --since N` | Retained events after N |
| `check NAME` | Request an immediate sample on an active worker |
| `pause NAME`, `resume NAME` | Stop/start worker execution |
| `remove NAME` | Stop and delete deployment runtime files |

Responses are JSON. The `data` field is a string containing serialized PDQL
JSON; parse it a second time to obtain rows. Updates carry sequence/initial/data;
responses report last_sequence/more/history_lost. Retrieve the latest data when
history is lost. Equality includes row values, count, and order; changes outside
the projection produce no notification.

Initial sampling advances the sequence without counting a change or invoking
an action unless `emit_initial` is enabled. Monitoring is polling and can miss
transient changes between samples. Failed loads/queries preserve the last
successful snapshot; errors and failed/timed-out worker states require inspection
and explicit resume.

## Daemon actions

```toml
[pagewatch]
directory = "/var/run/pagewatch"
action_script = "on_change.lua"
action_timeout_ms = 5000
emit_initial = false
```

Start with `pgwatchd --config Pagewatch.toml`; `--directory` overrides its root.
The action path is config-relative. Actions define `main(args)` returning 0
and receive name, data, sequence, and initial, with the watcher directory as
cwd. They run in separately supervised processes. Output/detailed errors are
suppressed. Actions serialize per watcher: at most four running and 32 pending
globally. Overflow increments action_dropped; failure/timeouts increment
action_failures. Action deadlines range from 10 to 300,000 ms.

## Bounds and recovery

Deployments live under `watchers/NAME/` with script/config, tab.json, and
state.toml. Bounds are 32 watchers, 64 control clients, 128 KiB per script/config,
1 MiB IPC frame, 256 KiB notification, and 16 events/512 KiB retained payload
per watcher. Partial clients expire after two seconds. Watchdogs cover Lua
coroutine/native hangs; workers terminate on abrupt daemon death.

Directories are 0700, files/socket 0600, peers require the daemon's effective
UID, and foreign-owner/symlink runtime directories are rejected. PDQL snapshots
are default-redacted. Scripts/actions are trusted same-user automation.

Deployments, last sanitized snapshot, and counters survive restart within the
directory lifetime and recover paused. Browser/Lua state, credentials, event
history, and pending actions are process-local. Snapshot and counters are
separately atomic file replacements. `/var/run` commonly disappears on reboot;
use a persistent override for retained deployments. `pgwatch.service` provides
a systemd account/RuntimeDirectory deployment example.

Reference: [pagewatch notes](../tools/pagewatch/README.md), `tools/pagewatch/`.

**Next:** [Spider](26-spider.md).
