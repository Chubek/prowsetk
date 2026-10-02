# Lua-driven crawler

`crawler` visits HTTP(S) links breadth-first, extracts selected page information
with the core PDQL engine, and composes the **ezlogin** and **scrape-endpoints**
Lua plugin APIs in the same authenticated session. It writes JSONL page records
and optionally merged OpenAPI YAML and Postman JSON. All requests, including
redirects and page-script requests, pass through the owning host session.

```sh
cmake --preset default
cmake --build --preset default
build/default/tools/crawler/crawler --config tools/crawler/Crawler.toml

# Network-independent extraction; no credential lookup or link following.
build/default/tools/crawler/crawler --config tools/crawler/Crawler.toml \
  --html '<h1>Offline</h1>' --output build/pages.jsonl
```

The tool builds on POSIX hosts when Lua and tomlplusplus are available. No new
production dependency is required. Its Lua modules are embedded in the binary;
installed executables work independently of the source checkout.

## Configuration

Paths in `Crawler.toml` are relative to that file. `--output` is relative to the
caller. See the shipped configuration for defaults.

| Table / setting | Meaning |
|---|---|
| `crawler.url` | HTTP(S) seed, without URL userinfo |
| `crawler.query` | PDQL projection for every visited page |
| `crawler.driver` | Optional trusted Lua script defining `main(args)` |
| `crawler.max_pages` | Attempt bound, 1–10,000 (default 100) |
| `crawler.max_depth` | Link depth, 0–32 (default 2) |
| `crawler.max_frontier` | Total queued URL bound, 1–100,000 (default 1,000) |
| `crawler.max_requests` | Shared quota for pages, login, robots, redirects and scripts, 1–100,000 |
| `crawler.respect_robots` | Honor applicable robots Allow/Disallow rules (default true) |
| `crawler.blocked_paths` | Path/query globs; default logout/signout/delete exclusions |
| `crawler.output`, `openapi`, `postman` | Output paths; the latter two are optional |
| `crawler.cookies_json` | Browser-exported cookie JSON, imported before live login |
| `crawler.api_only` | Filter non-API endpoints from specifications (default true) |
| `engine` | `javascript`, `timeout_ms`, `max_response_bytes`, `user_agent`, `proxy` |
| `arguments` | Extra scalar TOML values passed to a custom driver |

Fragments are removed for deduplication; relative links use core URL resolution.
Only same-origin page links enter the frontier. Login origins explicitly listed
in `login.trusted_origins` are also allowed at the transport boundary. Non-HTML
responses and HTTP failures are skipped. Robots uses `*` / `ProwseTk` groups,
longest matching Allow/Disallow rules, `*` and terminal `$`; it does not implement
crawl-delay or claim complete RFC 9309 compliance. Robots fetch failures,
401/403 and 5xx conservatively prevent automatic visits.

The summary reports successful pages, attempts, failures, `truncated`, `offline`,
and `complete: false`. Discovery remains heuristic even after frontier exhaustion.
Page records contain URL, title, depth and a PDQL result array. Output commits
use atomic owner-only files; a failed crawl does not publish a new result.
Page output and each specification are bounded to 16 MiB; at most 10,000 merged
method/path operations are retained, combining query parameter names and keeping
the higher-confidence representative provenance.

## Authentication and assistance

`[login]` accepts `mode` (`none`, `basic`, `bearer`, `api-key`, `custom-header`,
`cookie`, `form`, `oauth`), `url`, `header_name`, `username_field`,
`password_field`, `csrf_field`, `success_selector`, `trusted_origins`, and
`dotenv`. Secret values are looked up through `username_env`, `password_env`,
`token_env`, `value_env`, `cookie_env`, and `as_token_env`. Dotenv is read before
process environment lookup and never executed as shell code.

Form/OAuth flows first try imported cookies and require a successful HTTP
response plus an actual authenticated-only DOM control matching
`success_selector`. A cookie or words inside a script do not confirm login.
Session cookies remain in the scoped cookie jar. Private form values, script
content, sensitive attributes and sensitive query parameters are removed from
PDQL output; the scraper retains its normal provenance and redaction.

If login fails or challenge heuristics fire, an enabled `[assistant]` can run
either `command` (an executable, with the URL as one argument), or `project`
(a `Prowse.toml`) and `driver` (default `assist`). Approval is requested on stdin.
`PROWSETK_ASSISTANT_BROWSER` overrides `command`. Processes have a bounded
`wait_timeout_ms` and are launched with argv, never through a shell. An assistant
project declares a `url` argument and may export fresh cookies to the configured
cookie JSON. Installed assistant projects use the sibling `prowsetk` executable
for explicit crawler paths, or `prowsetk` on `PATH` for a `PATH` invocation.
The crawler then creates a fresh session and retries once. Browser
completion and assistant exit status are not authentication evidence.

## Lua and Booking.com

Custom drivers call `require('lcrawler').run(session, args)` and return its
integer code. An optional global `on_page(session, args)` can manipulate the DOM
before extraction. Drivers are trusted host automation, not sandboxes.

`booking-dotcom-admin/Crawler.toml` demonstrates cookie reuse, environment/dotenv
credentials, positive login evidence, bounded admin crawling and both scraper
exports:

```sh
build/default/tools/crawler/crawler \
  --config tools/crawler/booking-dotcom-admin/Crawler.toml
```

Place fresh browser-exported cookies in `booking-dotcom-admin/cookies.json` or
adjust that path. The default credential mode is HTML form login; `oauth` is
available when the account portal exposes its required token. MFA and human
verification may still need the configured assistant. Selectors and trusted
origins must match the account's actual page.

The `driver.lua` adapter and example `Prowse.toml` also support `prowsetk run`
(with `--html` for hermetic runs). Live adapters inherit their host's transport
policy; use the standalone `crawler` host for its enforced origin/request limits.
