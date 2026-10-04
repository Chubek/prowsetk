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
| `examples/booking-dotcom-admin-scrape/scrape-booking-dotcom-admin.lua` | Booking admin authentication, crawling, resolution/enrichment, OpenAPI/Postman |
| `tools/crawler/driver.lua` | Adapter to `lcrawler` |
| `tools/crawler/booking-dotcom-admin/driver.lua` | Booking crawler adapter with its example `Prowse.toml` |

Driver names are project declarations, not an automatic scan of `drivers/`.
Choose the matching example configuration with `--config` when running a driver
outside the root project.

## Booking.com example

```sh
prowsetk run booking-dotcom-admin \
  --config examples/booking-dotcom-admin-scrape/Prowse.toml \
  --html '<main><h1>Offline admin example</h1></main>' \
  --output build/booking-offline.yaml
```

The live workflow reads dotenv before `BOOKING_DOTCOM_USER` and
`BOOKING_DOTCOM_PASS`, tries imported session cookies first, and confirms login
with a successful response plus actual logout/account DOM controls. It follows
bounded trusted HTTPS redirects and crawls from the confirmed admin URL.
Human verification/MFA can require browser interaction and importing fresh
cookies. Cookie presence alone is not login evidence.

After crawling, the example composes restful-resolver and schema-grabber,
exports OpenAPI and Postman, applies API-only filtering by default, and records
incomplete heuristic coverage. Its bounded script and API-link scans can expose
references outside the set observed during page execution. Beacon and assistant
options are documented by the example's source and project declaration.

For agent-selected interactions, use the Booking launcher:

```sh
scripts/run-scrape-booking.sh \
  --output _scraped/booking-dotcom-admin/api.yaml \
  --postman _scraped/booking-dotcom-admin/api.postman_collection.json \
  --max-steps 24 --max-get-probes 64
```

It starts an OpenCode V2 server and invokes `ptk-opencode-marionette`. The
driver's `prepare_session(args)` prepares login on the host-bound managed
session, retaining its cookies and page context for C++ control. A trusted
`marionette-decisions.json` policy defines links, tabs, details and pagination;
`--decisions FILE` customizes selectors. Discovery and schemas accumulate
before and after actions through scrape-endpoints and schema-grabber. Live
GET probes are bounded, other methods are never probed, and both exports
retain redaction, provenance and explicitly incomplete coverage.
Automatic probes skip logout/sign-out-like paths to preserve the login session.
The launcher defaults to the Firefox handoff and Joe Litty Rooms XPath beacon;
`--assistant-browser-force false` reuses a confirmed imported session without
forcing that handoff. `--html HTML` uses an in-memory page transport and zero
GET probes while still contacting the separate OpenCode server.

`--verbose`/`-v` (`--verbse` alias) reports the failing stage/error code,
request/proxy route, response status/size/timing, imported-cookie counts and
login evidence checks. It omits page values, URL queries, credentials and raw
JavaScript/model errors. The launcher retains its private server log on failure
or a verbose run. `HTTPS_PROXY`/`HTTP_PROXY` and their lowercase aliases route
page requests; loopback bypasses proxies for the local OpenCode server.
`FIREFOX_PROFILE_DIR` selects the exact profile used for cookie capture. The
helper includes live WAL data, preserves JSON boolean flags and atomically
replaces an owner-only export. Failed/empty capture or import stops the run;
a forced handoff is not automatically repeated during the same preparation.

The separate standalone Booking crawler configuration is documented in
Chapter 24. Its transport quotas and TOML options belong to that host.

## Developing drivers

Prefer named option tables and normal module APIs to duplicating extraction or
authentication logic. Keep an offline HTML example. A new shipped driver gets
CTest integration coverage in `test_drivers.cpp` and a `prowsetk run` case in
`test_cli.cpp`. Test failures and redaction with MemoryNetworkClient when a
workflow requires simulated login or redirect responses.

Reference: `drivers/`, example `Prowse.toml` files, driver and CLI integration tests.

**Next:** [Lua extensions](11-lua-extensions.md).
