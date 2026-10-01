# Spider: persistent, programmable headless crawlers

`ptkspiderd` supervises named Flatworm crawler workers. `ptkspiderctl` controls
them over an owner-only local Unix socket. Each worker owns a `Browser`, one
live `Session`, and an LMDB cache accessed through `third_party/lmdbxx`.
The native `libprowsetk_spider.so` also implements the version-2 plugin ABI.

## Build and launch

Requires Linux, LMDB, and `third_party/lmdbxx/lmdb++.h`. Dependency discovery
lives in `cmake/Dependencies.cmake`: system LMDB is preferred, with
`third_party/lmdb/libraries/liblmdb` as the source fallback. Populate dependencies
using the repository submodule workflow. `PROWSETK_BUILD_SPIDER=OFF` disables
the plugin. Missing dependencies disable it explicitly; there is no RAM-only
substitute. Lua drivers additionally require Lua; interactions require QuickJS.

```sh
cmake --preset default
cmake --build --preset default
export PATH="$PWD/build/default/plugins/spider:$PATH"
ptkspiderd
```

The daemon stays in the foreground, suitable for a user service. Default state
directory: `$XDG_STATE_HOME/prowsetk/spider`, otherwise
`$HOME/.local/state/prowsetk/spider`. Default socket:
`$XDG_RUNTIME_DIR/ptkspiderd.sock`, otherwise `/tmp/ptkspiderd-<uid>.sock`.
Both binaries accept `--socket PATH`; the daemon accepts `--directory DIR`,
`--max-spiders N` (default 16, maximum 64), and `--job-timeout-ms N` (default
30000). Existing cache directories must belong to the daemon owner and have
no group/other permission bits. The directory lock prevents competing owners
of the same workers. An active socket is never replaced.

`ptkspider.service` is a sample systemd **user** unit. Adjust `ExecStart` to
your installation prefix, copy it into `~/.config/systemd/user/`, then use
`systemctl --user enable --now ptkspider.service`. A restarted service recovers spiders
paused; explicitly resume them after any required authentication setup.

## Control and marionetting

```sh
ptkspiderctl ping
ptkspiderctl --name booking.com -- create https://www.booking.com/
ptkspiderctl --name booking.com -- navigate https://www.booking.com/
ptkspiderctl --name booking.com -- click button 1
ptkspiderctl --name booking.com -- type 'input[name=search]' 'Paris' 1
ptkspiderctl --name booking.com -- query 'a[href]'
ptkspiderctl --name booking.com -- xpath '//h1/text()'
ptkspiderctl --name booking.com -- status
```

The selector is a CSS selector, and the optional index is **one-based**, in
document order. Click and type use `Session::click_element` /
`Session::type_element`: full synthetic cascades, bounded microtask draining,
and queued page navigation. Hidden/disabled controls fail explicitly. Page
JavaScript remains QuickJS page scripting, separate from Lua automation.

| Command | Behavior |
|---|---|
| `ping`, `list`, `shutdown` | Global commands; omit `--name` |
| `create URL [MAX_PAGES DEPTH INTERVAL_MS]` | Create a paused spider, enqueue the seed |
| `crawl URL [MAX_PAGES DEPTH INTERVAL_MS]` | Start a new bounded crawl epoch on that origin |
| `watch URL SECONDS [MAX_PAGES DEPTH INTERVAL_MS]` | Crawl now, then revisit periodically after each epoch finishes |
| `unwatch` | Disable future scheduled epochs |
| `enqueue URL [DEPTH]` | Add a deduplicated frontier URL |
| `pause`, `resume` | Pause or resume at an operation boundary |
| `stop` | Terminate the worker, preserving its cache/frontier |
| `remove` | Terminate and delete the spider and its cache |
| `status` | State, current redacted URL, pages, pending URLs, errors, schedule |
| `load-html HTML [URL]` | Install offline HTML into the live session |
| `navigate URL` | Explicit host-directed navigation |
| `click SELECTOR [INDEX]` | Synthetic click |
| `type SELECTOR TEXT [INDEX]` | Synthetic typing |
| `query SELECTOR`, `xpath EXPRESSION` | Inspect a sanitized copy of the current DOM |
| `driver FILE [KEY VALUE ...]` | Run a Lua driver against the existing session |

`create` defaults: 1000 successful pages, depth 5, 1000 ms between frontier
operations. Maximums: 10000 pages, depth 32, 10000 deduplicated URLs, and 1 MiB
of URL bytes per epoch. `watch` intervals range from 1 second to 30 days.
Frontiers are breadth-first. Fragments are removed; URL normalization and
same-origin deduplication are applied. A fresh `crawl` resets the epoch's queue,
seen set, and counters, retaining the cache and live authentication state.
`watch` runs bounded epochs rather than growing an unlimited seen set.

Automatic crawls read bounded `robots.txt` before fetching pages. Supported
rules: `User-agent: ProwseTkSpider` or `*`, `Allow`, `Disallow`, wildcard `*`,
terminal `$`, and longest matching path with Allow winning ties. Rules are
cached for an hour; 404/410 mean no rules. Other failed robots responses fail
closed with bounded retries. This is a restricted robots implementation;
percent-encoding equivalence, Crawl-delay, sitemaps and full RFC 9309 conformance
are not implemented. Explicit navigation, clicks and drivers are host-directed
automation rather than automatic robots-aware frontier visits.

Pages are fetched as GET through the owning `Session`; only successful responses
are installed and discovered. Links are resolved against the document base URL.
429, server failures and transport errors receive up to three retries with
exponential backoff. Numeric `Retry-After` values up to one hour are honored;
HTTP-date values use the normal backoff. Requests have 10-second timeouts,
2-MiB response limits, and at most five redirects. **Every actual transport
request**, including page-script requests and redirect hops, is restricted to
the spider's configured HTTP(S) origin. Credential-bearing URLs are rejected.
Loopback origins are usable for local applications; the daemon is a trusted
host tool, not a remote service.

## Lua spider drivers

Drivers define `main(args)` and return integer `0` on success. Arguments are
string-valued key/value pairs. The controller reads the file locally and sends
its bytes, preserving argument boundaries without invoking a shell. Each
invocation gets a fresh Lua runtime bound to the spider's **existing session**.

```lua
local spider = require("lspider")

function main(args)
    if args.html then
        spider.load_html(args.html, "https://example.com/")
    end
    local document = spider.document()
    spider.cache.put("last-title", document:title())
    for _, link in ipairs(document:links()) do
        -- Supply an absolute same-origin URL to enqueue().
        local href = link:attribute("href")
        if href:match("^https://example%.com/") then
            spider.enqueue(href, 1)
        end
    end
    return 0
end
```

`lspider` exposes `session()`, `document()`, `load_html(html, url)`,
`navigate(url)`, `click(selector, index)`, `type(selector, text, index)`,
`enqueue(url, depth)`, and `cache.get(key)` / `cache.query(prefix)` /
`cache.put(key, string_value)`. `session()` is a normal managed `lprowse`
session, so existing session, DOM, XPath, cookie and request APIs are available.

Cache reads are a bounded snapshot of up to 128 lexicographically sorted
`record/` entries and 1 MiB total per invocation; staged writes are visible
immediately to that driver. Use controller pagination for larger collections.
Frontier/cache writes are staged, validated, and atomically committed only after
`main` succeeds. Browser interactions are immediate and cannot be rolled back.
Driver output and detailed error strings are suppressed to avoid leaking
credentials; callers receive a categorical `lua_error` on failure. Driver
instructions are bounded; the process supervisor also handles coroutine/native
hangs. Drivers are trusted owner code, **not a Lua security sandbox**.

## Queryable cache

```sh
ptkspiderctl --name booking.com -- cache query page/ 20
ptkspiderctl --name booking.com -- cache get page/000
ptkspiderctl --name booking.com -- cache select 'a[href]' 20
ptkspiderctl --name booking.com -- cache put crawl-note 'first pass complete'
ptkspiderctl --name booking.com -- cache query record/ 20
ptkspiderctl --name booking.com -- cache query record/ 20 record/crawl-note
```

Prefix queries return `[{"key": ..., "value": ...}]` in key order. The optional
`AFTER_KEY` is an exclusive pagination cursor. `cache select` runs the shared
CSS engine over cached sanitized documents and returns per-page matches. Queries
return at most 100 rows and bounded bytes; pass the last returned key to continue.
`cache get` retrieves one known key. Internal frontier/state keys are inaccessible
through these commands.

Each worker has a durable, fully synchronized 256-MiB LMDB environment. Cache
content is bounded: 256 page slots in a circular history, at most 1000 driver
records, keys up to 128 bytes, values up to 64 KiB. Page records contain a
redacted URL, title, status (`0` for host actions), timestamp and sanitized HTML.
HTML above 256 KiB is omitted, explicitly marked `content_omitted`. Page slots
are numbered `page/000` through `page/255`; use timestamps, not slot order, for
chronology after wraparound. Full maps or disk errors pause the worker and retain
the last committed frontier. There is no unlimited map growth.

Cookies, authorization headers and raw response bodies are not exported to the
cache. Sanitization removes scripts/styles/textarea contents, form-control values,
sensitive attributes and inline event handlers, and redacts sensitive URL query
parameters. Sensitive record keys (password/token/secret/cookie/header names)
lose their values. Arbitrary prose and application-defined record values can
still contain private data: this is structural redaction, not a universal secret
detector. The local cache and socket are owner-only; that owner is trusted.

## Longevity and recovery

- The daemon polls clients and worker channels without blocking on browser work.
  One operation is in flight per worker; concurrent operations return `busy`.
  Other spiders and `ping` remain responsive during a stalled driver.
- IPC uses a four-byte big-endian frame size and length-delimited string fields;
  maximum frame size is 4 MiB, with at most 64 connected clients. Partial clients
  expire after five seconds. No command is interpreted by a shell.
- Workers send heartbeats. A missed watchdog deadline kills only that worker;
  subsequent control relaunches it paused. A disconnected client does not cancel
  a dispatched operation. Avoid blindly retrying mutating actions after an IPC
  failure; a lost acknowledgement does not prove the action was not applied.
- Worker address space is bounded to 1 GiB in normal builds (ASan uses its own
  virtual address layout). Worker termination does not block daemon shutdown.
- The durable frontier keeps the current task until the page, discovered links
  and updated state commit together. Interrupted GETs are replayable, giving
  at-least-once fetch behavior rather than exactly-once guarantees.
- SIGINT/SIGTERM and `shutdown` preserve committed data. Workers are also
  terminated if the supervisor dies. Restart recovers registered workers paused,
  including watch schedules. Use `resume` to continue.
- Cookies, live JavaScript state, headers and browser storage remain in the live
  process and **are not recovered across restarts**. The last sanitized page is
  restored for inspection. Re-run the authentication/setup driver before resuming
  an authenticated crawl.

## Native plugin and embedding

The shared plugin can be loaded with `PluginRegistry::load_native`. It reports
spider capabilities without starting a daemon implicitly. Optional
`cache_directory` configuration enables a host-specific metadata cache: the
document hook writes the latest redacted URL to `record/latest-url`. It does
not receive or export session credentials. All C ABI errors are converted to
status codes; ABI version remains 2. Daemon operations are explicit through
the controller or the C++ `control()` function.

The build-tree `prowsetk_spider_static` library exposes `Cache`, `Engine`, and
`control()` through `include/prowsetk/plugins/spider.hpp`. `Engine` and `Cache`
are single-owner components; `tick()` performs at most one frontier operation
and `command()` processes a bounded command. Network clients can be replaced
with `MemoryNetworkClient` for hermetic embedding and tests. The installed
native plugin provides the C ABI; the static embedding library is build-tree only.

## Tests

```sh
ctest --preset default -R spider --output-on-failure
ctest --preset asan -R spider --output-on-failure
```

Unit tests cover durable transactions, map exhaustion, private permissions,
protocol parsing, origin policy, robots rules, frontier recovery, bounded crawl
epochs, redaction, Lua staging, scheduled revisits and native plugin loading.
The daemon integration test drives the real controller, performs synthetic
clicks, runs Lua drivers, checks malformed/slow clients, kills a hung coroutine,
and verifies isolation and restart recovery without public network access.
