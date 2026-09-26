# beacon

Native ProwseTk oracle plugin that bridges ProwseTk drivers to Firefox
through `beacond` and a Native Messaging host. Firefox is the server;
Beacon is the client. A **Flash** is a driver request for browser-side
assistance (live DOM, network info, stylesheet, or a tracepoint
subscription). See `AGENTS.md` for the lifecycle and message protocol.

- **Flash lifecycle:** request → registration (seeking) → addon discovery →
  user-approved connect → data exchange / tracepoint pushes → disconnect.
  `FlashSessionManager` owns this state machine; `beacond` multiplexes
  concurrent Flashes from multiple drivers.
- **User consent:** the addon sends page data only after an explicit
  "Connect to Flash" click, and only for the connected tab (scope).
  A Flash existing never proves the page was observed.
- **Security:** the Native Messaging manifest allowlists the addon id;
  `beacond` gates socket access with owner-only permissions. Tokens, cookies,
  and page credentials are secret-bearing. Data payloads are delivered
  verbatim after consent; generated Flash lists redact sensitive URL queries.
- **Transport:** local IPC only (Unix socket). No TCP, no WASI, no arbitrary
  sockets. All automation network access stays host-mediated through
  `NetworkClient`.

Discovery is heuristic and never authoritative.

## Layout

- `include/prowsetk/plugins/beacon.hpp`, `src/beacon.cpp` — protocol,
  Native Messaging framing, `FlashSessionManager`, redaction.
- `src/plugin_entry.cpp` — native ABI (`PROWSETK_PLUGIN_ABI_VERSION` 2).
- `src/beacond_main.cpp` — `beacond` daemon (Unix-socket Flash broker).
- `src/beaconctl.cpp` — driver-side local Flash request and polling command.
- `src/beacon_native_host.cpp` — Firefox stdio bridge to `beacond`.
- `lua/beacon.lua` — spec layer (`new_flash`, `render_request`, validation).
- `prowsetk-beacon-addon/` — WebExtension (`manifest.json`, `background.js`,
  `content.js`, `sidebar.html`/`sidebar.js`).
- `beacon-native-host/` — manifest + install notes for the native host.
- `beacond/` — service notes for the daemon.

## Lua

```lua
local beacon = require("plugins.beacon.lua.beacon")
local flash = beacon.new_flash("page_dom", {
    url_pattern = "https://example.com/*",
    resource_types = { "main_frame" },
}, "11111111-1111-4111-8111-111111111111")
print(beacon.render_request(flash))
```

## Build

```bash
cmake --preset default
cmake --build --preset default
ctest --preset default
```

`beacond` and `beacon-native-host` build with the default preset; no extra
toolchain is required. The WIT contract for a future WASM component lives in
`wit/beacon.wit`.

## Firefox addon (.xpi)

`prowsetk-beacon-addon/` packs into a deterministic, unsigned
`prowsetk-beacon-addon.xpi` via `scripts/pack-beacon-addon.py`
(sorted entries, fixed timestamps, `0644` modes):

```bash
cmake --build --preset default --target beacon-addon-xpi
# artifact: build/default/plugins/beacon/prowsetk-beacon-addon.xpi
python3 scripts/pack-beacon-addon.py \
  --check build/default/plugins/beacon/prowsetk-beacon-addon.xpi \
  --native-host-manifest plugins/beacon/beacon-native-host/prowsetk_beacon.json
```

The check validates the ZIP layout, the manifest (stable
`browser_specific_settings.gecko.id` of `prowsetk-beacon@example.com`,
permissions, background/sidebar wiring), and that the id matches the
`allowed_extensions` allowlist in `beacon-native-host/prowsetk_beacon.json`.
Load the `.xpi` in Firefox via `about:debugging → Load Temporary Add-on`
for development, or sign it through Mozilla for distribution. The CMake
`install` also ships the `.xpi` next to the unpacked addon sources.

## Testing

```bash
./build/default/plugins/beacon/beacond --socket /tmp/beacond.sock
# Load prowsetk-beacon-addon in Firefox (about:debugging → Load Temporary Add-on)
build/default/plugins/beacon/beaconctl flash page_dom 'https://example.com/*'
# In Firefox, open the Beacon sidebar, List Flashes, Connect to Flash
```

Unit coverage (`tests/test_beacon.cpp`, labels `unit;plugin;beacon`)
exercises request-type parsing, validation, deterministic JSON rendering,
secret redaction, Native Messaging framing, the full Flash lifecycle,
expiry, capacity, and the native ABI entry point. The optional Lua and Node
tests check wire URL filters, addon consent and tab scope; a local socket
integration test exercises `beacond`, `beaconctl`, and the framed native host.
No public network is required; temporary sockets stay in the test binary
directory.

## Local Flash exchange

Start `build/default/plugins/beacon/beacond --socket /tmp/beacond.sock`, then
use `build/default/plugins/beacon/beaconctl [--socket PATH] flash page_dom
'https://example.com/*'`. The command prints the assigned Flash ID. Open an
HTTP(S) tab in Firefox and use **List Flashes** then **Connect to Flash** in the
sidebar. Click **Send Page**, **Send Network Info**, or **Send Stylesheet** and
retrieve the next message with `beaconctl poll ID`. An empty queue returns
`{"type":"flash_empty"}`; `beaconctl disconnect ID` removes a Flash.
`beaconctl ping` and `beaconctl list` inspect the local broker. The broker
accepts one Flash per connection and queues at most 16 data messages; it does
not persist data. Polling is nonblocking and the driver must supply its own
bounded waiting policy. This command can be called from a driver process; the
Lua specification layer currently only formats messages and has no IPC binding.

The broker socket is owner-only (`0600`) and rejects an occupied non-socket
path. Processes running as the socket owner can impersonate either side of a
Flash: the addon click is the user consent control, not an authentication
boundary against other same-user programs. Do not expose the socket to other
users. `beacon.auth_token` in plugin configuration has no broker counterpart
and cannot authenticate a connection. Page HTML and styles can contain secrets
and are delivered verbatim after consent. Network capture begins on connect,
contains only method, resource type, and URL origin/path (no query or body),
and is limited to 500 entries. Navigation or closing the connected tab revokes
the addon connection. Tracepoints watch DOM mutations for a CSS selector and
report a bounded set of changed node names; request and style tracepoint
watchers and full HAR capture are not implemented.
