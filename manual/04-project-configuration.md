# Chapter 04: Project Configuration

[Manual index](README.md)

## A minimal project

Save this as `Prowse.toml` next to a `driver.lua` defining `main(args)`:

```toml
[project]
name = "local-scrape"
version = "0.1.0"
root = "."

[engine]
javascript = false
follow_redirects = true
max_redirects = 5

[network]
timeout_ms = 10000

[[drivers]]
name = "inspect"
script = "driver.lua"
entrypoint = "main"
enabled = true
arguments = [
  { name = "url", type = "url", default = "https://example.test/" },
  { name = "html", type = "string" },
  { name = "output", type = "path" }
]
```

Run it with `prowsetk run inspect --config Prowse.toml --html '<h1>Local</h1>'`.
Chapter 10 supplies the complete driver body.

## Cloudflare Browser Run and OAuth

`ptk-browser-run` and the C++ Browser Run client consume `[cloudflare].account_id`
and `[cloudflare].api_token`; nonempty `CLOUDFLARE_ACCOUNT_ID` and
`CLOUDFLARE_API_TOKEN` override them. These credentials are control-plane inputs,
not page request headers. An API token needs Browser Rendering – Edit permission.

`ptk-oauth-assist` consumes `[oauth]`: `client_id`, `scopes`,
`authorization_endpoint`, `token_endpoint`, and `redirect_uri`.
`PROWSETK_OAUTH_CLIENT_ID` / `PROWSETK_OAUTH_SCOPES` override the first two.
Cloudflare endpoints default to `https://dash.cloudflare.com/oauth2/auth` and
`https://dash.cloudflare.com/oauth2/token`; the default redirect URI is
`http://localhost:8976/oauth/callback`. Supply a provider-registered public client
and approved scopes/redirect. Login prints an authorization URL and accepts the
complete redirect URL through hidden terminal input, then caches tokens under
`$HOME/.cache/ProwseTk/OAuth`. With no explicit API token, Browser Run uses this
Cloudflare-bound cache. Neither a token's existence nor OAuth completion proves
Browser Run authorization.

See [Chapter 36: Browser Run](36-cloudflare-browser-run.md) and
[Chapter 37: OAuth assistance](37-oauth-assistance.md) for CLI commands, remote
snapshot semantics, bounds and cache permissions. Ordinary `prowsetk run` does
not initiate either service automatically.

## Paths and arguments

`project.root` resolves relative to the configuration file's directory unless
it is absolute. Driver scripts and autoloaded plugin paths resolve under that
root. A configured cookie JSON path also resolves under the project root;
`--cookies-json FILE` uses the command-line path. Output arguments remain
strings passed to the driver; they are not automatically rebased or created.

Declared argument types are `string`, `integer`, `boolean`, `path`, and `url`.
Required arguments must be supplied unless a default is present. Unknown
arguments and disabled/unknown drivers are errors. `secret = true` identifies
secret-bearing arguments; drivers must keep those values out of prints,
generated artifacts, and error messages.

## Configuration consumption

`ProjectConfig` is a declarative parsed model. The current `run` command
consumes a subset directly:

| Section | Current `run` behavior |
|---|---|
| `[project]` | Root and project metadata |
| `[engine]` | JavaScript, identity, redirects, unsupported-API behavior, network observation |
| `[network]` | Request timeout and proxy selection |
| `[[drivers]]` | Entrypoint, script, enablement, arguments |
| `[[plugins]]` | Load enabled/autoloaded native, Lua, or WASM registrations; initialize registry |
| `[sessions]` | Import `cookies_json` before the driver runs |
| `[assistant-browser]` / `[assistant_browser]` | Supply handoff settings as driver arguments |

The model also parses `[[extensions]]`, `[[commands]]`, `[plugin_config.NAME]`,
`[lua]`, `[security]`, and endpoint-extraction settings. The CLI does not
automatically execute extension/preload files, configure native plugins from
`plugin_config`, dispatch custom commands, apply all declared sandbox/security
settings, or provide disk persistence simply because those tables exist.
Applications consume these settings explicitly. Lua plugin registrations record
metadata; load executable Lua code through the runtime or `dofile`.

## Plugins and assistants

```toml
[[plugins]]
name = "scrape-endpoints"
type = "native"
path = "build/default/plugins/scrape-endpoints/libprowsetk_scrape_endpoints.so"
enabled = true
autoload = true

[assistant-browser]
enabled = false
command = "firefox"
method = "webdriver"
wait_timeout_ms = 300000
```

Adjust paths to the selected project root and platform. Native initialization
and native configuration are separate operations. Chapter 21 shows
`PluginRegistry::configure_all`; plugin Lua APIs accept their own option tables.

`$PROWSETK_ASSISTANT_BROWSER` overrides the browser command in supporting
handoff workflows. A method/endpoint setting describes a handoff but does not
itself import browser cookies or confirm login. The standalone crawler has its
own `Crawler.toml` and assistant-project execution contract.

## Parsing and overrides

`load_project_config(path)` and `parse_project_config(contents)` expose the
same model to C++. Invalid TOML or missing configuration fails with an error;
without tomlplusplus, parsing reports unsupported functionality. Unknown
project keys are generally ignored for forward compatibility. Crawler,
pagewatch, and TUI configurations have their own stricter validators.

`run --user-agent VALUE`, `--proxy URL`, and `--cookies-json FILE` override the
corresponding host settings. TOML strings are not a general environment-variable
expansion language. Read credentials explicitly from environment/dotenv through
the chosen authentication workflow.

Reference: `project_config.hpp`, `src/core/project_config.cpp`, CLI driver loader.

**Next:** [C++ embedding](05-cpp-embedding.md).
