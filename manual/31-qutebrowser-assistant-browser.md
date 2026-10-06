# Chapter 31: Qutebrowser Assistant Browser

[Manual index](README.md)

## From a browser tab to a ProwseTk driver

`tools/qutebrowser-bridge` connects an explicitly selected Qutebrowser tab to a
ProwseTk Lua driver. Qutebrowser supplies the current page HTML; Flatworm loads
that snapshot for DOM queries, PDQL, IR emission, and plugin composition. The
assistant browser remains a separate, user-owned process.

```text
Qutebrowser tab
  → userscript (QUTE_HTML / QUTE_URL)
  → private Unix socket broker
  → lquteipc + qutebrowser_bridge.lua
  → JavaScript-disabled Flatworm Session
  → queries, endpoint discovery, schema enrichment, exports
```

The bridge uses Qutebrowser's documented `:spawn --userscript` interface.
Python scripts use the standard library. A small native Lua module supplies
AF_UNIX IPC and private file I/O. Flatworm's native plugin ABI and page network
transport are independent of this assistant connection.

| Executable | Use |
|---|---|
| `ptk-qute-bridge` | Start the broker, inspect status, finish a conversation |
| `ptk-qute-send` | Publish one current-tab snapshot with purpose `page` |
| `ptk-qute-scrape` | Publish one current-tab snapshot with purpose `scrape` |
| `ptk-qute-marionette` | Publish the initial snapshot and accept two-way actions |

The `scrape` purpose is a tag for the receiving driver. Extraction happens when
Lua calls the existing scraper API, as described in Chapter 32.

## Build and installation

The scripts require POSIX and Python 3.9+. Interactive use also requires
Qutebrowser with userscript support. The existing Lua development dependency
enables the `lquteipc` build target; no additional linked library is introduced.

```sh
cmake --preset default
cmake --build --preset default
```

Source-tree scripts are executable directly from `tools/qutebrowser-bridge/`.
The native module is `build/default/tools/qutebrowser-bridge/lquteipc.so`.
An installation places these components under its configured prefix:

| Path below the prefix | Contents |
|---|---|
| `share/prowsetk/qutebrowser-bridge/` | Four executable scripts, Python helpers, README |
| `share/prowsetk/qutebrowser-bridge/lua/` | Lua companion and strict JSON helper |
| `lib/prowsetk/lua/lquteipc.so` | Native IPC module; library directory can vary |
| `share/prowsetk/examples/booking-dotcom-admin-api/` | Example project, driver, action policy |

Use absolute userscript paths or link the executables into Qutebrowser's
`~/.local/share/qutebrowser/userscripts/` directory. Keep the Python helper files
beside the actual executables. Installed Lua callers select the native module
with `ipc_module` or configure `package.cpath`; Chapter 32 documents module
selection and the companion API.

## Start one conversation

From the repository root, start a broker for the page's approved origin:

```sh
tools/qutebrowser-bridge/ptk-qute-bridge serve \
  --directory build/qute-demo --url https://example.test/ \
  --timeout 300 --max-actions 8
```

The broker creates an owner-only directory, `bridge.sock`, and `bridge.json`.
It prints a value-free readiness message. The URL must be HTTP(S), without
userinfo. Its origin is fixed for the conversation; paths and query strings do
not widen that origin.

| `serve` option | Default | Meaning |
|---|---|---|
| `--directory` | required | Private runtime directory |
| `--url` | required | URL defining the approved origin |
| `--timeout` | 300 | Lifetime in seconds, 1–3600 |
| `--max-actions` | 256 | Total accepted action budget, 0–256 |
| `--launch` | off | Ask for approval and launch Qutebrowser at the URL |
| `--browser` | `qutebrowser` | Executable name/path used by `--launch` |

`--launch` takes terminal approval; EOF cancels it. The executable and URL are
separate argv entries, and browser diagnostics are suppressed. A manually
opened browser works with the same broker. The driver and broker do not need
to own the browser's lifetime.

After a receiving Lua driver is ready, invoke a sender from the desired tab:

```text
:spawn --userscript /absolute/prowsetk/tools/qutebrowser-bridge/ptk-qute-send --bridge /absolute/prowsetk/build/qute-demo/bridge.json
```

Substitute actual absolute paths. Run from command mode, with the target tab
active. Hint-mode invocations are rejected. `PROWSETK_QUTE_BRIDGE`, when inherited
by Qutebrowser, supplies the descriptor path instead of `--bridge`; the explicit
option takes precedence. The capability token stays in the private descriptor.

For a persistent key binding in `config.py`:

```python
config.bind(',ps', 'spawn --userscript /absolute/prowsetk/tools/qutebrowser-bridge/ptk-qute-scrape --bridge /absolute/prowsetk/build/qute-demo/bridge.json')
```

One-shot senders exit after publication. A capture may arrive before the
driver starts; the broker retains its latest snapshot. Chapter 34 supplies a
ready-to-run receiving project for `https://admin.booking.com`.

## Status, completion, and restart

```sh
tools/qutebrowser-bridge/ptk-qute-bridge status \
  --bridge build/qute-demo/bridge.json
tools/qutebrowser-bridge/ptk-qute-bridge finish \
  --bridge build/qute-demo/bridge.json
```

Status reports revision, connection state, and action counters without page
values. `finish` closes the conversation and wakes a waiting marionette. The
broker process continues until its lifetime expires or it receives
SIGINT/SIGTERM; orderly shutdown removes the socket and descriptor.

Start a new broker for a new conversation. An occupied runtime is refused.
After an abrupt kill, explicitly remove stale `bridge.sock` and `bridge.json`
once the old process has stopped. A second broker must not evict a live one.

## Protocol and data contract

The descriptor contains `version`, `socket`, `token`, `origin`, and optional
`bulk: "fifo-v1"`. Its directory
is `0700`, and descriptor/socket modes are `0600`. One socket connection carries
one newline-terminated JSON request and reply. Requests include version 1,
the token, and an operation; replies include `ok`.

Operations are `publish`, `snapshot`, `attach`, `next`, `act`, `detach`, `status`,
and `finish`. The Python and Lua companions implement this exchange. Duplicate
members, unknown fields/actions, invalid versions, and over-budget messages are
rejected. Linux additionally checks the peer UID. Same-user programs that can
read the descriptor are trusted peers.

Snapshots contain full `url`, verbatim `html`, `tab`, `purpose`, and a
broker-assigned `revision`. Action captures additionally contain `action_id`
and `action_status`. Only the latest capture is retained in broker memory.
Snapshots can contain private page values; the broker does not log or persist
their HTML. Apply the query/export policies in Chapter 32 before writing data.

New senders and Lua clients transfer snapshot bodies through private named
FIFOs in the runtime directory on both local hops. The socket carries the
generated FIFO basename and byte count; a FIFO frame is `QHTML1\n`, an unsigned
64-bit big-endian length, the raw UTF-8 bytes, then EOF. Short/extra bodies,
mismatched lengths, unsafe file kinds/permissions, symlinks and stalled peers
fail with value-free errors. Normal exit unlinks transfer FIFO nodes. This
avoids JSON-escaping overhead and the old 4-MiB snapshot cap; legacy inline
messages remain supported within the encoded-message bound.

| Bound | Value |
|---|---|
| Simultaneous clients / attached marionettes | 8 / 1 |
| FIFO HTML / encoded control or legacy inline message | 16 MiB / 8 MiB |
| JSON nesting | 32 levels |
| Snapshot/action wait | At most 30 seconds |
| Partial-client read timeout | 5 seconds |
| Selector / fill value | 4096 UTF-8 bytes each |
| Marionette capture delay | 0–5000 ms; default 500 |
| Private output file | 16 MiB |
| Bulk transfer deadline | 10 seconds per hop, in addition to the control wait |

The first attachment or capture pins the tab index. Keep that tab active in
the same window, and do not reorder or close tabs during a marionette run.
The userscript API supplies an index, not a stable tab ID; a replacement
same-origin tab at the same index cannot be distinguished.

The bridge transfers DOM snapshots. It supplies no cookie jar, HTTP status,
response bytes, or historical network trace. A confirmed-looking page is DOM
evidence, not verified HTTP authentication. Qutebrowser's page requests and
redirects obey its own policies. Origin checks restrict accepted captures and
explicit bridge actions, not all networking performed by the assistant browser.

Reference: [bridge notes](../tools/qutebrowser-bridge/README.md),
`tools/qutebrowser-bridge/protocol.py`, `broker.py`, `userscript.py`,
[Qutebrowser userscript documentation](https://qutebrowser.org/doc/userscripts.html).

**Next:** [Lua snapshots and API discovery](32-lua-snapshots-and-api-discovery.md).
