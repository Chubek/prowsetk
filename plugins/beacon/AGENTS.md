# AGENTS.md -- Beacon Plugin

Beacon is not only a plugin for ProwseTk. It is also a plugin for browsers. From one side, Beacon is `ProwseTk Beacon`, a plugin for Firefox and Chrome, that remains in communication with `Beacon`, a plugin for ProwseTk.

The browser side of Beacon uses the capability of browser addons to communicate with systerm resources via Stdio, and the ProwseTk side of Beacon sends orders and receives data. On both sides, this is a bidirectional communication.

The browser side of Beacon exposes a menu with the following options:
- Establish Connection
- Close Connection
- Export Cookies
- Export IndexedDB
- Export Local Storage
- Export Session Storage
- Export Authentication

When a connection is established, instead of 'Export', you can 'Send' these info to ProwseTk:

- Send Cookies
- Send IndexedDB
- ...

Also, other options:
- Send Network History
- Send Memory Footprint
- Debug
- Puppeteer
- Execute Command




