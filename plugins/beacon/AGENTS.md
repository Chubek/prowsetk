# AGENTS.md -- Beacon Plugin

Beacon is a ProwseTk plugin that uses Firefox's Native Messaging capability through its addons to establish a client-server relationship between the browser resources and ProwseTk drivers. In this case, Firefox is the server, and the Beacon plugin is the client, acting as an oracle for other plugins and drivers in general.

## The Flash

A "Flash" is a message sent from ProwseTk, using Beacon plugin, to the Firefox Native Messaging binary through `beacond`, the daemon for the system. `beacond` uses IPC to let the Native Messaging bridge know a ProwseTk driver is in need of assistance.

## The Addon

Beacon's Firefox addon is called "ProwseTk Beacon". It is a sliding menu with the following buttons:
- **List Flashes**: lists the currently seeking Flashes;
- **Connect to Flash**: connects to a Flash;
- **Send Page**: sends the entire page to the Flash;
- **Send Network Info**: sends the entire network info to Flash (requires network info to be loaded);
- **Send Stylesheet**: sends the entire stylesheet to the Flash;
- **Set Tracepoint**: marks a DOM element or network request for monitoring; future changes trigger automatic Flash notifications;
- **Disconnect**: closes the current Flash connection and returns to the list view.

## Architecture

```
ProwseTk Driver → beaconctl (IPC) → beacond → Native Messaging Host → Firefox Addon
                                                                              ↓
                                                                         Browser DOM/Network APIs
```
### Components

**beacond**
The system daemon that bridges ProwseTk and Firefox. It:
- Listens for Flash requests from Beacon plugin over IPC (Unix socket or named pipe).
- Maintains active Flash sessions with unique identifiers.
- Relays messages between the Native Messaging host and ProwseTk drivers.
- Handles Flash lifecycle (creation, active connections, timeouts, cleanup) and bounded message queues read by `flash_poll`.

**Native Messaging Host**
A small binary registered with Firefox that:
- Implements the stdio-based Native Messaging protocol (4-byte length prefix + JSON).
- Communicates with `beacond` over IPC.
- Translates between Firefox addon messages and `beacond` protocol.

**ProwseTk Beacon Addon**
A WebExtension with:
- Background script managing Native Messaging connections and Flash state.
- Content scripts injected into pages to extract DOM, computed styles, and network data.
- Sidebar UI presenting the Flash list and action buttons.
- Permissions: `nativeMessaging`, `tabs`, `webRequest`, `webRequestBlocking`, `<all_urls>`, `activeTab`.

## Flash Lifecycle

1. **Request**: A driver process invokes `beaconctl flash TYPE URL_PATTERN` to create a Flash; the Lua specification layer has no live IPC binding yet.
2. **Registration**: Beacon plugin sends the Flash to `beacond`, which assigns it a unique ID and marks it "seeking."
3. **Discovery**: The addon polls or receives a push notification of seeking Flashes via the Native Messaging host.
4. **Connection**: User clicks "Connect to Flash" in the addon. The addon sends a connect message; `beacond` marks the Flash "connected" and notifies the driver.
5. **Data exchange**: The user clicks a send button (page DOM, request metadata, or stylesheet). The addon sends the result to a bounded queue; the driver polls with `beaconctl poll ID`.
6. **Tracepoint**: A CSS selector watcher can send scoped DOM mutation metadata without an explicit send button after connection.
7. **Termination**: Either side can disconnect. `beacond` cleans up the Flash session and notifies the other party.

## Message Protocol

### Flash Request (Driver → beacond)
```json
{
  "type": "flash_request",
  "flash_id": "uuid-v4",
  "request_type": "page_dom" | "network_info" | "stylesheet" | "tracepoint",
  "filters": {
    "url_pattern": "https://example.com/*",
    "resource_types": ["main_frame", "stylesheet", "script"]
  }
}
```
### Flash List (Addon ← Native Host ← beacond)
```json
{
  "type": "flash_list",
  "flashes": [
    {
      "flash_id": "uuid-v4",
      "request_type": "page_dom",
      "filters": { "url_pattern": "https://example.com/*" },
      "status": "seeking" | "connected"
    }
  ]
}
```
### Connect (Addon → Native Host → beacond → Driver)
```json
{
  "type": "flash_connect",
  "flash_id": "uuid-v4",
  "tab_id": 42
}
```
### Data Response (Addon → Native Host → beacond → Driver)
```json
{
  "type": "flash_data",
  "flash_id": "uuid-v4",
  "data_type": "page_dom",
  "payload": {
    "html": "<html>...</html>",
    "url": "https://example.com/page",
    "timestamp": 1727155200
  }
}
```
### Tracepoint Event (Addon → Native Host → beacond → Driver)
```json
{
  "type": "tracepoint_event",
  "flash_id": "uuid-v4",
  "event": "dom_mutation" | "network_request" | "style_change",
  "details": { "selector": "#element", "changes": [...] }
}
```
The addon includes `tab_id` in `flash_data` and `tracepoint_event` so the
broker can reject messages for another tab. A driver sends
`{"type":"flash_poll","flash_id":"uuid-v4"}` and receives the next queued
message verbatim or `{"type":"flash_empty"}`. The queue holds at most 16
messages and closes with the Flash. Polling does not wait for new data.
## Security Considerations

- **Allowlist**: The Native Messaging manifest restricts connections to the official ProwseTk Beacon addon ID.
- **Authentication**: the daemon creates an owner-only Unix socket (`0600`); programs running as that owner share access. Token authentication is not implemented.
- **User consent**: The addon requires explicit user action (clicking "Connect to Flash") before sending page data; no automatic exfiltration.
- **Scope**: The addon accesses only the connected tab after the click and disconnects on navigation or tab close. It does not monitor all browsing activity.

## Installation

1. Install the ProwseTk Beacon addon from the Firefox Add-ons store or load it temporarily for development.
2. Install `beacond` and the Native Messaging host binary using the ProwseTk installer or package manager.
3. Register the Native Messaging manifest at the appropriate OS location (see Native Messaging documentation); no installer is shipped.
4. Start `beacond` as a system service or user daemon.
5. Verify connectivity with `beaconctl ping` from the command line.

## Use Cases

- **Live DOM inspection**: Drivers can request the exact rendered DOM and computed styles without launching a full browser automation session.
- **Network inspection**: Capture bounded request method, resource type, and URL origin/path metadata from the connected tab after consent; no headers, bodies, timing, or full HAR.
- **Dynamic tracing**: Set a CSS selector to watch DOM mutations; request and stylesheet tracepoint watchers are not yet available.
- **Stylesheet extraction**: Retrieve all applied CSS rules for a page, useful for theme analysis or rendering reproduction.
- **Cross-driver oracle**: Multiple ProwseTk drivers can issue Flashes concurrently; `beacond` multiplexes bounded queues for them.

## Limitations

- Requires Firefox with the addon installed and `beacond` running; no support for other browsers without equivalent Native Messaging implementations.
- The user must manually connect to a Flash; fully automated workflows need user interaction at connection time.
- Network info capture begins on connect; previously issued requests are unavailable.
- Tracepoints add performance overhead; monitoring high-frequency mutations can slow the page.

## Development

**Addon source**: `prowsetk-beacon-addon/`
- `manifest.json`: WebExtension manifest declaring permissions and background script.
- `background.js`: Manages Native Messaging port and Flash state.
- `content.js`: Extracts DOM, styles, and network data from pages.
- `sidebar.html` / `sidebar.js`: UI for Flash list and actions.

**Native Messaging host**: `src/beacon_native_host.cpp` and `beacon-native-host/` manifest.
- C++ binary implementing the stdio protocol and IPC with `beacond`.

**beacond**: `beacond/`
- Daemon managing Flash sessions and IPC with the Native Messaging host and Beacon plugin.

**Build**: `cmake --preset default && cmake --build --preset default`.

**Testing**:
```bash
# Start beacond
./beacond --socket /tmp/beacond.sock

# Load addon in Firefox (about:debugging → Load Temporary Add-on)

# Issue a Flash from ProwseTk
build/default/plugins/beacon/beaconctl flash page_dom 'https://example.com/*'

# In Firefox, open the Beacon sidebar, click "List Flashes", then "Connect to Flash"
```
