# beacond

Flash session daemon bridging Beacon clients and the Firefox Native
Messaging host (`plugins/beacon/AGENTS.md`).

- Binary: `beacond` (built from `../src/beacond_main.cpp`).
- Listens on a Unix socket (`--socket`, default `/tmp/beacond.sock`) for
  newline-delimited JSON Flash protocol messages.
- Maintains Flash lifecycle: creation (seeking), user-approved connections,
  timeouts (`--timeout-ms`), capacity (`--max-flashes`), cleanup.
- Replies are newline-delimited JSON; `flash_list_request` returns the
  current `flash_list` for addon discovery.
- `flash_data` and `tracepoint_event` are accepted for a connected Flash's
  tab and queued until `flash_poll` returns the next message (or `flash_empty`).

```bash
./beacond --socket /tmp/beacond.sock --timeout-ms 30000 --max-flashes 64
```

Authentication is local socket permissions (`0600`, owner-only). There is no
broker token authentication. Same-user processes with socket access can
impersonate a client; keep the socket private. Data is not logged.
