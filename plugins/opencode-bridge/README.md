# opencode-bridge

Native ProwseTk extension plugin (ABI v2) bridging Flatworm sessions and an
OpenCode local HTTP/SSE server (default `http://127.0.0.1:4096`).

## Capabilities

- **Prompt-driven scraping**: send a sanitized page snapshot plus extraction
  goals/JSON schema to an OpenCode agent session, validate the JSON-object
  answer.
- **Autonomous navigation loop**: expose `click`/`fill`/`navigate`/`evaluate`
  as tool names; the agent replies with the next action and the driver
  executes it through `prowsetk::Element` / `prowsetk::Session`.
- **DOM & endpoint introspection**: sanitized HTML/text snapshots for agent
  context. Endpoint discovery itself stays in `scrape-endpoints`; the bridge
  only relays snapshots.

## Configuration

`[plugin_config.opencode-bridge]` entries (`configure`) or environment:

| Key | Env | Default |
|---|---|---|
| `base_url` | `OPENCODE_BASE_URL` | `http://127.0.0.1:4096` |
| `username` | `OPENCODE_SERVER_USERNAME` | empty (`opencode` against real servers) |
| `password` | `OPENCODE_SERVER_PASSWORD` | empty |
| `timeout_ms` | — | `30000` |
| `max_requests` | — | `16` |
| `prompt_wait_ms` | — | `120000` (cap on waiting for an agent reply) |
| `api_prefix` | — | `""` (set `"/api"` for real OpenCode v2 servers) |
| `allow_remote_http` | — | `false` (plain HTTP is loopback-only unless set) |

HTTP Basic auth is used only when a username or password is set. A real
`opencode serve` prints a `server password` line on startup and expects Basic
`opencode:<password>` (`www-authenticate: Basic`); export that password and
leave the username at `opencode`. Loading the plugin is network-free; every
OpenCode call is an explicit client method over the host `NetworkClient`.
Redirects are rejected, inputs/responses are size-bounded, session ids are
validated (`.`/`..` rejected, query-encoded), SSE `data:` lines buffer per
event, and errors never echo credentials or page content.

## Real OpenCode v2 servers (`api_prefix = "/api"`)

The default (`""`) speaks the bare bridge paths from AGENTS.md
(`/session`, `/session/:id/message`, `/event`, `/session/:id/abort`). Real
OpenCode v2 servers mount everything under `/api`, so point the client at
them with `api_prefix = "/api"` (the booking driver does this by default):

- `create_session` → `POST /api/session`, id read from the `data.id` envelope.
  C++ `create_session(true)` installs a deny-all tool permission policy for
  decision-only sessions, as used by opencode-marionette.
- `prompt` → `POST /api/session/:id/prompt`, then polls the session message
  list (newest first, limit 128) until a new completed assistant message appears (reasoning parts
  excluded, text parts joined). Bounded by `prompt_wait_ms` and the request
  budget; a failed agent run raises instead of returning stale text. The
  `tools` argument is rejected here — the v2 prompt route takes text only.
- `prompt_async` → `POST /api/session/:id/prompt`, returns the message id.
- `stream_events` → `GET /api/event` with the same SSE parsing.
- `abort` → `POST /api/session/:id/interrupt`.

## Endpoint cleanup

`build_endpoint_cleanup_prompt(endpoints_json[, instructions])` (C++ and
`lopencode.build_cleanup_prompt`) builds the shared subtractive cleanup
prompt over a **redacted** JSON array of endpoint objects. The contract is
enforced on both sides:

- The prompt orders the agent to return ONLY a subset of the input objects,
  copied verbatim — never invented, normalized, or rewritten URLs/methods.
- Callers must intersect the answer with the scraped `(method, url)` set and
  keep original records verbatim; the booking driver
  (`examples/booking-dotcom-admin-scrape`) implements exactly this, with empty
  keep-lists and non-array answers rejected.

```lua
local prompt = lopencode.build_cleanup_prompt(endpoints_json, "prefer v2 APIs")
local answer = assert(client:prompt(session_id, prompt))
```

## Lua (`lopencode`)

```lua
local lopencode = require("lopencode")
local client = assert(lopencode.client.new({
    base_url = "http://127.0.0.1:4096",
    username = os.getenv("OPENCODE_SERVER_USERNAME"),
    password = os.getenv("OPENCODE_SERVER_PASSWORD"),
}))

local session = assert(client:create_session())
local answer = assert(client:prompt(session, "List the article titles", {"click", "navigate"}))
local op = assert(client:prompt_async(session, "Crawl the next 10 pages"))
local n = assert(client:stream_events(session, function(event) print(event) end))
assert(client:abort(session))

-- One-call scrape of an lprowse session/document (or snapshot table):
local json = assert(client:scrape_with_prompt(prowse_session, "Get the headings"))
assert(client:close())
```

With a second argument to `new`, a Lua transport callback
`function(request) -> {status, body}` replaces the socket client (hermetic
tests, host-mediated routing). While a transport call is active the client is
busy: `close` or reentry from the callback fails instead of deadlocking.

## DOM hygiene

Snapshots are sanitized on detached parses: heavy multimedia, inline base64,
and excessive SVG are stripped; semantic tags (`nav`, `main`, `article`,
`table`, `form`, `a`, `button`) and `data-*` attributes are kept. Documents
over 100KB fall back to a title/text summary. Never treat agent output as
authoritative; validate before acting on it.

C++ `OpenCodeClient::prompt_message(id, text)` additionally supports the documented
`opencode serve` synchronous text-parts protocol (`POST /session/:id/message`),
returning concatenated assistant text parts and rejecting assistant errors.
It disables common built-in tools in the request; server-side permissions remain
caller-owned. It shares authentication, route prefix and finite transport budgets.
Existing `prompt` and Lua callers retain their legacy and `/api` polling behavior.
