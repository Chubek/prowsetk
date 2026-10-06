# Chapter 10: Drivers

[Manual index](README.md)

## Complete offline-capable driver

Save the following as `driver.lua` and use the `inspect` declaration from
Chapter 4. Add a declared `query` string argument if you want CLI query overrides.

```lua
local prowse = require('lprowse')
local pdql = require('lpdql')

function main(args)
    local browser = prowse.browser.new()
    local session = browser:create_session()
    if args.html then
        session:load_html(args.html, args.url)
    else
        session:navigate(args.url)
    end
    local data = pdql.query(session, args.query or 'select tag, text from <h1>', 'json')
    if args.output and args.output ~= '' then
        local file = assert(io.open(args.output, 'wb'))
        file:write(data, '\n')
        file:close()
    else
        print(data)
    end
    session:close()
    return 0
end
```

Run `prowsetk run inspect --html '<h1>Local heading</h1>'`. The JSON result is
`[{"tag":"h1","text":"Local heading"}]`. Create the output parent directory
before using an output path with this simple driver.

## Driver contract

A declaration selects a script and entrypoint, conventionally `main(args)`.
The host supplies a table of named, type-coerced arguments. Return an integer
exit code, with 0 for success. A raised error propagates to the host invocation.
Use `args.html` as the optional network-independent path for shipped drivers.

Drivers are trusted automation. They can use host Lua libraries and resources
available to the process. Secrets from arguments, environment, cookie files,
and forms must remain outside printed diagnostics and generated records.
An error should explain the stage that failed without interpolating credentials.

`prowsetk run` binds a Browser; the driver normally creates its own session.
Crawler, pagewatch, and spider instead provide an already managed session or
tool-specific accessor. Follow each host's lifecycle and staging contract.

## Shipped drivers

| Driver / location | Purpose |
|---|---|
| `drivers/crawl_site.lua` | Same-origin, depth-bounded crawl and JSONL page metadata |
| `drivers/login.lua` | Form login, session cookie reuse, redacted login summary |
| `examples/booking-dotcom-admin-api/api.lua` | Qutebrowser admin snapshots, optional two-way actions, OpenAPI/Postman |
| `tools/qutebrowser-bridge/lua/console.lua` | Interactive console controller behind `ptk-qute-repl`; not a `Prowse.toml` driver |
| `tools/crawler/driver.lua` | Adapter to `lcrawler` |
| `tools/crawler/booking-dotcom-admin/driver.lua` | Booking crawler adapter with its example `Prowse.toml` |

Driver names are project declarations, not an automatic scan of `drivers/`.
Choose the matching example configuration with `--config` when running a driver
outside the root project.

## Booking.com snapshot example

```sh
build/default/src/cli/prowsetk run booking-admin-api \
  --config examples/booking-dotcom-admin-api/Prowse.toml \
  --html '<script>fetch("/api/reservations?limit=2")</script>' \
  --output build/booking-offline.yaml \
  --postman build/booking-offline.postman.json
```

The current project loads a Qutebrowser DOM snapshot into a JavaScript-disabled
Session and composes scrape-endpoints with schema-grabber. Its optional
`html` argument is hermetic. Live operation requires positive DOM login
evidence; the userscript interface supplies no cookies, HTTP status, or response
bodies. Outputs retain redaction, provenance, and incomplete-coverage metadata.

[Chapter 34](34-booking-admin-api-snapshots.md) provides the live/offline recipes,
argument reference, evidence selectors, trusted action policies, and export
metadata. The standalone Booking crawler's cookie/HTTP authentication and
transport quotas are documented in [Chapter 24](24-crawler.md).

The former `examples/booking-dotcom-admin-scrape` project has been removed.
Its Firefox/OpenCode launcher defaults refer to that earlier project's
preparation script and decisions file; use a configured current project rather
than treating those defaults as a shipped Booking workflow.

## Developing drivers

Prefer named option tables and normal module APIs to duplicating extraction or
authentication logic. Keep an offline HTML example. A new shipped driver gets
CTest integration coverage in `test_drivers.cpp` and a `prowsetk run` case in
`test_cli.cpp`. Test failures and redaction with MemoryNetworkClient when a
workflow requires simulated login or redirect responses.

Reference: `drivers/`, example `Prowse.toml` files, driver and CLI integration tests.

## Qutebrowser assistant projects

`examples/booking-dotcom-admin-api/Prowse.toml` declares `booking-admin-api`,
which consumes a current-tab snapshot from the Qutebrowser userscripts in
`tools/qutebrowser-bridge`. `ptk-qute-scrape` sends one snapshot;
`ptk-qute-marionette` also accepts bounded Lua DOM actions and returns fresh
captures. The driver uses JavaScript-disabled Sessions with scrape-endpoints
and schema-grabber, supports offline `html`, and writes redacted, explicitly
incomplete specifications. Live export requires DOM login evidence; the
userscript interface does not expose HTTP status, cookies or response bodies.
See the [project workflow](../examples/booking-dotcom-admin-api/README.md) and
[bridge API](../tools/qutebrowser-bridge/README.md) for setup, actions and limits.
For repeated exploration of an authenticated tab, `ptk-qute-repl` provides a
Replxx-backed persistent Lua console over the same conversation instead of a
finite driver run: see [Chapter 38](38-interactive-qute-console.md).

The manual's complete guide starts with
[Chapter 31](31-qutebrowser-assistant-browser.md), followed by
[Lua composition](32-lua-snapshots-and-api-discovery.md),
[marionette actions](33-qutebrowser-marionettes.md), the
[Booking project](34-booking-admin-api-snapshots.md), the
[interactive console](38-interactive-qute-console.md), and the
[snapshot transport](39-snapshot-bulk-transport.md).

**Next:** [Lua extensions](11-lua-extensions.md).
