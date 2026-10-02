# Chapter 24: Crawler

[Manual index](README.md)

## First run

`crawler` is a POSIX Lua/TOML automation host. It visits page links breadth-first,
extracts PDQL page data, and composes ezlogin and scrape-endpoints in the same
session. It can publish JSONL pages plus merged OpenAPI and Postman artifacts.
Lua modules are embedded, so the installed executable works independently of
the source checkout.

```sh
crawler --config tools/crawler/Crawler.toml \
  --html '<h1>Offline crawl</h1>' --output build/pages.jsonl
```

Usage is `crawler --config FILE [--html HTML | --html-file FILE] [--output FILE]`.
The default configuration path is `Crawler.toml`; `--help` prints usage.
Offline runs skip credential lookup, robots requests, and link following. Page
scripts can mutate supplied HTML but make no resource/network requests.

## A small configuration

```toml
[crawler]
url = "https://example.com/"
query = 'select tag, text from xpath("//main//h1 | //main//h2")'
max_pages = 100
max_depth = 2
max_frontier = 1000
max_requests = 1000
respect_robots = true
api_only = true
output = "output/pages.jsonl"
openapi = "output/openapi.yaml"
postman = "output/postman.json"

[engine]
javascript = true
timeout_ms = 10000
max_response_bytes = 4194304

[login]
mode = "none"
```

Paths in TOML resolve relative to the configuration file, including drivers,
dotenv, cookie imports, assistants, and outputs. A CLI `--output` path resolves
relative to the caller. Unknown tables/settings and unsupported argument types
are rejected. The seed must be HTTP(S) without userinfo.

| Setting | Default | Accepted bound/meaning |
|---|---|---|
| `max_pages` | 100 | 1–10,000 page attempts |
| `max_depth` | 2 | 0–32 link depth |
| `max_frontier` | 1000 | 1–100,000 total queued URL bound |
| `max_requests` | 1000 | 1–100,000 actual transport requests |
| `blocked_paths` | logout/signout/delete patterns | Path/query globs |
| `engine.timeout_ms` | 10000 | 1–120,000 ms |
| `engine.max_response_bytes` | 4 MiB | 1 byte–16 MiB |
| `cookies_json` | empty | Browser-exported cookie file |
| `driver` | empty | Optional custom Lua script |

`engine.user_agent` and `engine.proxy` are also supported.

## Traversal and transport policy

Fragments are removed for deduplication; relative links use the core resolver.
Only same-origin page links enter the frontier. Non-HTML and failed HTTP
responses are skipped. Every transport operation, including login, robots,
redirects, and page scripts, shares the request quota. The host permits the
seed origin and explicitly configured login/trusted origins.

Robots supports `ProwseTk`/`*` groups, longest matching Allow/Disallow rules,
wildcard `*`, and terminal `$`. Failed robots fetches, 401/403, and 5xx responses
conservatively block automatic visits. Crawl-delay and full RFC 9309 behavior
are outside this restricted implementation. Blocked-path exclusions and robots
decisions apply to automatic scheduling.

## Authentication

`[login].mode` accepts none/basic/bearer/api-key/custom-header/cookie/form/oauth.
Settings include `url`, `header_name`, username/password/CSRF field names,
`success_selector`, `trusted_origins`, and `dotenv`. Secret values are looked
up using `username_env`, `password_env`, `token_env`, `value_env`, `cookie_env`,
and `as_token_env`. Dotenv precedes process-environment lookup and is parsed
without shell execution.

Form/OAuth login first tries imported cookies and requires a successful response
plus an authenticated-only DOM control. Script strings and cookie presence do
not satisfy that check. Scope credential injection to the configured origins.

## Assistant projects and browsers

```toml
[assistant]
enabled = true
project = "Assistant/Prowse.toml"
driver = "assist"
wait_timeout_ms = 300000
```

Alternatively set `command = "firefox"`; it is an executable name/path with the
URL supplied as one argument. `$PROWSETK_ASSISTANT_BROWSER` overrides command.
Approval is requested on stdin. Processes run with argv and a bounded deadline.
Assistant projects declare a `url` argument and can export fresh cookies to the
configured cookie file. Browser-command users are prompted to finish/export
before continuing.

After successful assistance, the host creates a fresh session and retries once.
Positive login evidence is still required. Explicit crawler paths prefer a
sibling `prowsetk`; a PATH invocation resolves `prowsetk` on PATH.

## Custom drivers, output, and Booking.com

```lua
function main(args)
    return require('lcrawler').run(session, args)
end
```

The host supplies `session`; optional `on_page(session, args)` runs before
extraction. Extra scalar `[arguments]` values reach the driver. Reserved host
arguments cannot be overwritten there. Use the supplied session to preserve
the guarded transport.

Page JSONL contains URL, title, depth, and a PDQL result array. Summary fields
are `pages` (successful pages), `attempts`, `failures`, `truncated`, `offline`,
and `complete: false`.
Each artifact is bounded to 16 MiB; merged specifications retain up to 10,000
method/path operations, union query names, and prefer higher-confidence
provenance. Outputs are atomically replaced owner-only files after success;
the multiple artifact writes are not one database transaction.

`tools/crawler/booking-dotcom-admin/Crawler.toml` demonstrates imported cookies,
dotenv/environment credentials, authenticated DOM evidence, admin traversal,
and both exports. Its `driver.lua`/`Prowse.toml` adapter also works with
`prowsetk run`. Use the standalone host for its enforced origin/request limits.
Selectors/trusted origins must match the actual account page; MFA/human
verification may require fresh browser cookies.

Reference: [crawler notes](../tools/crawler/README.md), `tools/crawler/lua/lcrawler.lua`.

**Next:** [Pagewatch](25-pagewatch.md).
