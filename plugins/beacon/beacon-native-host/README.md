# beacon-native-host

Firefox Native Messaging host bridging the ProwseTk Beacon addon and
`beacond` (`plugins/beacon/AGENTS.md`).

- Binary: `beacon_native_host` (built from `../src/beacon_native_host.cpp`).
- Protocol: Firefox stdio framing (4-byte LE length + JSON) on stdin/stdout;
  newline-delimited JSON over the `beacond` Unix socket.
- Security: local IPC only. The Firefox manifest allowlists the official
  addon id; page data passes through verbatim and is never logged.

## Manifest (Linux example)

```json
{
  "name": "prowsetk_beacon",
  "description": "ProwseTk Beacon native host",
  "path": "/usr/lib/prowsetk/beacon_native_host",
  "type": "stdio",
  "allowed_extensions": ["prowsetk-beacon@example.com"]
}
```

Install to `~/.mozilla/native-messaging-hosts/prowsetk_beacon.json`
(per-user) or `/usr/lib/mozilla/native-messaging-hosts/` (system-wide).
