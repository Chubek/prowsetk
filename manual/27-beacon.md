# Chapter 27: Beacon

[Manual index](README.md)

## Browser assistance through Firefox

Beacon bridges ProwseTk drivers to a user-selected Firefox tab. A **Flash**
requests page DOM, bounded network metadata, stylesheets, or a DOM tracepoint.
The addon lists seeking Flashes and sends data only after the user explicitly
connects one to a matching HTTP(S) tab.

```text
Driver → beaconctl → beacond → Native Messaging host → Firefox addon
```

Flatworm remains the engine for host automation. Firefox supplies optional,
consented assistance through a separate local IPC workflow.

## Installing the addon and host

Build the normal preset and optional packaged addon:

```sh
cmake --build --preset default --target beacon-addon-xpi
python3 scripts/pack-beacon-addon.py \
  --check build/default/plugins/beacon/prowsetk-beacon-addon.xpi \
  --native-host-manifest plugins/beacon/beacon-native-host/prowsetk_beacon.json
```

The XPI is deterministic and unsigned. Load it temporarily using Firefox
`about:debugging` for development; signed distribution is a separate Mozilla
step. Installation ships binaries `beacond`, `beaconctl`, and
`beacon_native_host`, plus addon data under `share/prowsetk/beacon/`.

Register a per-user Native Messaging manifest at
`~/.mozilla/native-messaging-hosts/prowsetk_beacon.json`, adjusting the absolute
binary path to the installation:

```json
{
  "name": "prowsetk_beacon",
  "description": "ProwseTk Beacon native host",
  "path": "/opt/prowsetk/bin/beacon_native_host",
  "type": "stdio",
  "allowed_extensions": ["prowsetk-beacon@example.com"]
}
```

The addon ID must match that allowlist. The host speaks Firefox's four-byte
little-endian length-prefixed JSON protocol on stdio and newline-delimited JSON
to the broker. Registration is a manual installation step.

## Flash exchange

Start `beacond --socket /tmp/beacond.sock`, then from another terminal:

```sh
beaconctl --socket /tmp/beacond.sock ping
beaconctl --socket /tmp/beacond.sock flash page_dom 'https://example.com/*'
```

The response identifies the Flash. In Firefox, open the Beacon sidebar,
**List Flashes**, select the Flash, and **Connect to Flash** with the matching
tab active. Use **Send Page**, **Send Network Info**, or **Send Stylesheet**.
Retrieve a queued message using its returned ID:

```sh
beaconctl --socket /tmp/beacond.sock poll FLASH_ID
beaconctl --socket /tmp/beacond.sock disconnect FLASH_ID
```

`list` shows broker Flashes. An empty poll returns `{"type":"flash_empty"}`.
Polling is nonblocking; drivers supply a bounded waiting/deadline policy.
Queues contain at most 16 messages, and broker data is process-local.
`beacond` also accepts `--timeout-ms N` and `--max-flashes N`.

The Native Messaging host uses the default `/tmp/beacond.sock` unless its
socket override is configured; use the same path as the broker/controller.

## Scope and data contracts

| Data | Contract |
|---|---|
| `page_dom` | Page HTML from the connected tab, delivered verbatim after consent |
| `stylesheet` | Available style data; page/style content can contain secrets |
| `network_info` | Up to 500 method/resource-type/origin-path records, capture begins on connect |
| `tracepoint` | CSS-selector-scoped DOM mutation metadata |

Network data omits query strings, bodies, headers, timing, and full HAR.
Request/stylesheet tracepoint watchers are not implemented. Navigation and tab
closure revoke the addon connection. Broker messages include tab_id so data for
another connected tab is rejected.

## Lua and native APIs

The Lua file is a specification/validation layer:

```lua
local beacon = dofile('plugins/beacon/lua/beacon.lua')
local flash = beacon.new_flash('page_dom', {
    url_pattern = 'https://example.com/*', resource_types = {'main_frame'}
}, '11111111-1111-4111-8111-111111111111')
local request_json = beacon.render_request(flash)
```

It formats messages and has no live IPC binding. Use beaconctl from a driver
process or the C++ protocol/client components for actual registration/delivery.
The Booking example's `--flash-on true` path coordinates a network_info Flash;
its offline beacon JSON option supplies a hermetic input alternative.

The native plugin exposes version-2 metadata/configuration, and
FlashSessionManager manages lifecycle, capacity, expiry, connection and queues.
`wit/beacon.wit` is a future WASM contract.

## Local trust and troubleshooting

The broker socket is 0600 and shared by trusted programs running as its owner.
There is no token authentication; plugin auth_token configuration cannot
authenticate broker clients. Addon consent/tab scoping does not prevent another
same-user socket client from impersonating a protocol peer. Retain private
socket access and treat delivered page/style bytes as secret-bearing input.

If Flashes do not appear, check ping, manifest name/path/allowlist, addon ID,
broker/host socket alignment, and the active tab's URL match. Seeking/connected
state alone does not establish that page data was received or authentication
was confirmed.

Reference: [Beacon notes](../plugins/beacon/README.md),
[Native Messaging setup](../plugins/beacon/beacon-native-host/README.md).

**Next:** [Web and Python interfaces](28-web-and-python-interfaces.md).
