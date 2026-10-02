# Chapter 26: Spider

[Manual index](README.md)

## Persistent crawl workers

`ptkspiderd` supervises named crawler workers, each with its own Browser,
Session, bounded breadth-first frontier, and private LMDB cache. `ptkspiderctl`
controls workers over a local owner-only Unix socket. The native version-2
plugin is a separate registry integration.

Build requirements are Linux, LMDB, and lmdbxx; Lua enables drivers and
QuickJS enables synthetic interactions. `PROWSETK_BUILD_SPIDER=OFF` disables
the feature. The daemon stays in the foreground:

```sh
ptkspiderd
```

Defaults: state under `$XDG_STATE_HOME/prowsetk/spider` or
`$HOME/.local/state/prowsetk/spider`; socket at `$XDG_RUNTIME_DIR/ptkspiderd.sock`
or `/tmp/ptkspiderd-<uid>.sock`. Both programs accept `--socket PATH`; daemon
options include `--directory DIR`, `--max-spiders N` (16, maximum 64), and
`--job-timeout-ms N` (30,000).

## Controller reference

```sh
ptkspiderctl --name example -- create https://example.com/ 100 2 1000
ptkspiderctl --name example -- load-html '<h1>Offline</h1>' https://example.com/
ptkspiderctl --name example -- query 'h1'
ptkspiderctl --name example -- status
```

The `--` delimiter separates controller options from the worker command.

| Command | Behavior |
|---|---|
| `ping`, `list`, `shutdown` | Global control; omit name |
| `create URL [MAX_PAGES DEPTH INTERVAL_MS]` | Create paused worker and enqueue seed |
| `crawl URL [MAX_PAGES DEPTH INTERVAL_MS]` | Start a bounded new crawl epoch |
| `watch URL SECONDS [MAX_PAGES DEPTH INTERVAL_MS]` | Crawl and schedule later epochs |
| `unwatch`, `enqueue URL [DEPTH]` | Disable schedule or add frontier task |
| `pause`, `resume`, `stop`, `remove` | Pause/start, terminate retaining data, or delete worker/cache |
| `load-html HTML [URL]`, `navigate URL` | Explicit page loading |
| `click SELECTOR [INDEX]`, `type SELECTOR TEXT [INDEX]` | Synthetic actions; one-based match index |
| `query SELECTOR`, `xpath EXPRESSION` | Inspect sanitized current DOM |
| `driver FILE [KEY VALUE ...]` | Run trusted Lua against the existing session |
| `cache ...` | Cache get/query/select/put operations |

Default crawl bounds are 1,000 successful pages, depth 5, and 1,000 ms between
frontier operations. Maximums are 10,000 pages, depth 32, 10,000 deduplicated
URLs, and 1 MiB URL bytes per epoch. Watch intervals are 1 second–30 days.
New epochs reset frontier/seen/counters while retaining cache/live authentication.

## Robots, retries, and origin policy

Automatic GET visits check bounded robots rules for `ProwseTkSpider`/`*`, with
Allow/Disallow, wildcard `*`, terminal `$`, and longest-match/Allow tie-breaking.
Rules cache for an hour; 404/410 mean no restrictions, other failures fail
closed with bounded retries. Explicit navigation, drivers, and clicks are
host-directed actions rather than automatic frontier visits.

Transport failures, 429, and server failures get up to three retries with
backoff. Numeric Retry-After up to one hour is honored. Requests have 10-second
timeouts, 2-MiB responses, and five redirect hops. Every actual request,
including page scripts and redirects, stays on the configured HTTP(S) origin;
userinfo URLs are rejected. Local loopback applications are supported.

## Lua driver staging

```lua
local spider = require('lspider')
function main(args)
    if args.html then spider.load_html(args.html, 'https://example.com/') end
    spider.cache.put('last-title', spider.document():title())
    return 0
end
```

`lspider` provides session/document, load_html/navigate, click/type,
enqueue, and cache.get/query/put. `session()` returns managed lprowse userdata.
Arguments are string-valued key/value pairs. Each driver gets a fresh LuaRuntime
bound to the existing browser session; the controller sends script bytes without
shell interpretation.

Frontier/cache writes are staged and atomically committed after main returns 0.
Browser actions are immediate. Driver cache reads are a snapshot of up to 128
sorted `record/` entries/1 MiB, with staged writes visible to that invocation.
Output/detailed errors are suppressed; failures report categorical lua_error.
Instruction bounds and process watchdogs cover hangs, including coroutines.

## Cache access and persistence

```sh
ptkspiderctl --name example -- cache query page/ 20
ptkspiderctl --name example -- cache select 'a[href]' 20
ptkspiderctl --name example -- cache put note 'first pass'
ptkspiderctl --name example -- cache query record/ 20
```

Prefix queries return key/value rows in key order, with an optional exclusive
AFTER_KEY cursor. Controller queries are limited to 100 rows/bounded bytes.
Use page timestamps rather than circular slot order for chronology.

Each fully synchronized LMDB environment is 256 MiB: 256 page slots,
1,000 driver records, 128-byte keys, 64-KiB record values. Page HTML above
256 KiB is omitted with content_omitted. Sanitized pages remove private
controls, scripts/styles, sensitive attributes/handlers, and private URL
components. Cookies/headers/raw responses are not cache exports. Arbitrary
record prose still belongs to the trusted owner.

The current frontier task remains until its page, links, and state commit
together. Interrupted GETs may be replayed: at-least-once fetching. Disk/map
failures pause the worker with committed frontier intact. Nonblocking daemon
IPC stays responsive independently of browser work; one operation per worker
is in flight. An IPC failure does not prove a mutating operation was unapplied.

Restarts recover workers/watch schedules paused and restore the last sanitized
page for inspection. Cookies, headers, JavaScript/Lua state, and browser storage
do not recover; rerun authentication before resume. Partial clients expire after
five seconds. `ptkspider.service` is a systemd user-service example.

## Native helper integration

The native shared plugin reports metadata without implicitly starting a daemon.
Optional cache_directory enables a document hook storing latest redacted URL.
The build-tree static API exposes Cache, Engine, and control(); use replacement
NetworkClients for hermetic embedding. This static library is build-tree-only.

Reference: [spider notes](../plugins/spider/README.md),
`plugins/spider/include/prowsetk/plugins/spider.hpp`.

**Next:** [Beacon](27-beacon.md).
