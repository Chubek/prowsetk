# AGENTS.md — plugins/opencode-bridge

## Purpose & Scope
`plugins/opencode-bridge` is a native ProwseTk extension plugin conforming to `ProwseTk-Plugin.h` and `lprowsext`. It establishes a bi-directional communication bridge between **ProwseTk** (Flatworm engine, DOM traversal, network interception, Lua scripts) and **OpenCode** (local HTTP/SSE REST API, default `http://127.0.0.1:4096`).

This bridge enables:
1. **Prompt-driven Scraping**: Directing OpenCode agents to autonomously inspect, parse, and extract structured data from pages loaded in ProwseTk sessions.
2. **Dynamic Scraper Generation**: Generating, hot-reloading, and executing `lprowse` / `lprowsext` / PDQL scripts derived from OpenCode session reasoning.
3. **DOM & Endpoint Introspection**: Relaying live DOM snapshots, network interception records, and OpenAPI endpoint discoveries (`/api/endpoints`) to OpenCode context.
4. **Endpoint Cleanup**: Sending a redacted endpoint list for a subtractive agent cleanup pass (drop non-API junk, keep the rest verbatim). The agent may only remove entries; callers intersect the answer with the scraped `(method, url)` set so invented URLs can never enter the specs.

---

## Endpoint Cleanup Contract

`build_endpoint_cleanup_prompt(endpoints_json[, instructions])` (C++ and
`lopencode.build_cleanup_prompt`) builds the shared cleanup prompt. Rules,
enforced by the prompt text and by every caller:

- Input is a redacted JSON array of endpoint objects; budgets apply before any
  request is built.
- The answer must be a JSON array (or an object wrapping one under
  `endpoints`/`keep`) containing only a subset of the input objects.
- Empty keep-lists, non-array answers, non-endpoint items, and over-budget
  answers are rejected; the caller keeps its seeds and reports the shortfall.
- `examples/booking-dotcom-admin-scrape` applies this after its api-only
  filter and before schema enrichment, recording the outcome in an
  `x-prowsetk-opencode` metadata block (`used`, `kept`, `dropped`,
  `invented-ignored`, `note`).

---

## Architecture & Integration Points

### 1. ProwseTk Host Bindings
- **Plugin Entry**: Implements lifecycle hooks declared in `include/prowsetk/ProwseTk-Plugin.h`.
- **Lua Bindings (`lopencode`)**: Registers a new Lua module inside `LuaRuntime` exposing:
  - `opencode.client.new({ base_url, password })`
  - `client:prompt(session_id, text, tools)`
  - `client:prompt_async(session_id, text)`
  - `client:stream_events(session_id, on_event_callback)`
  - `client:scrape_with_prompt(prowse_session, prompt_instructions, schema)`
  - `lopencode.build_cleanup_prompt(endpoints_json[, instructions])` (pure function, no transport)
- **DOM & Network Context Handlers**: Hooks into `Session::current_document()` and `NetworkClient` interception filters to export sanitized HTML, text, and discovered endpoints to OpenCode.

### 2. OpenCode IPC Client Interface
The bridge talks to OpenCode's local REST/SSE server using `libcurl` or ProwseTk's internal HTTP client:
- `POST /session` — Spawn dedicated agent sessions for scraping jobs.
- `POST /session/:id/message` — Send synchronous scrape instructions with page state.
- `POST /session/:id/prompt_async` & `GET /event` — Asynchronous streaming workflows for long crawls or pagination.
- `POST /session/:id/abort` — Cancel in-flight scraping operations if Flatworm times out or encounters fatal security blocks.
- **Authentication**: Inspects `OPENCODE_SERVER_PASSWORD` and `OPENCODE_SERVER_USERNAME` env variables (HTTP Basic Auth). Default base URL: `http://127.0.0.1:4096`. Plain HTTP is loopback-only unless `allow_remote_http` is set; HTTPS is unrestricted.

Real OpenCode v2 servers mount the API under `/api` (`POST /api/session`,
`POST /api/session/:id/prompt`, `GET /api/session/:id/message`,
`GET /api/event`, `POST /api/session/:id/interrupt`) and challenge Basic auth
(username `opencode`, server-printed password). Set `api_prefix = "/api"` to
speak those routes: `prompt` then polls the message list until a new
completed assistant message arrives (bounded by `prompt_wait_ms` and the
request budget), and `tools` is rejected since the v2 prompt route takes text
only. `scripts/run-scrape-booking.sh` uses this path through the marionette
controller. Decision-only C++ callers use `create_session(true)` to install a
V2 deny-all tool permission policy; ordinary session creation retains its
default behavior. Poll bounded newest messages and reject assistant errors.
Plain HTTP is loopback-only unless `allow_remote_http` is set; HTTPS is unrestricted.

---

## Coding Guidelines & Conventions

### Language & Modern Standards
- **C++**: C++20 standard. Use RAII, smart pointers (`std::unique_ptr`, `std::shared_ptr`), and `std::span` / `std::string_view` where applicable.
- **No Direct Socket Calls**: Route all HTTP client calls through ProwseTk's networking abstractions or a minimal HTTP/1.1 client wrapper conforming to ProwseTk error-handling conventions.
- **Lua Compatibility**: Ensure all Lua C API calls are safe, stack-balanced, and compatible with Lua 5.4.
- **Thread Safety**: Never mutate a `prowsetk::Session` or its `Document` DOM across thread boundaries. Dispatch agent callbacks to the session's worker thread.

### DOM Sanitization & Prompt Hygiene
- Always strip heavy multimedia, inline base64 assets, and excessive SVG definitions before packaging the DOM into an OpenCode message prompt.
- Retain semantic tags (`nav`, `main`, `article`, `table`, `form`, `a`, `button`, data attributes).
- For large documents (>100KB HTML), prefer summarizing via PDQL projections or document text extracts (`document:text()`) instead of raw outer HTML.

---

## Core Scenarios & Workflows

### Scenario 1: Prompt-Driven Document Extraction
When the user executes a prompt-driven scrape:
1. ProwseTk navigates to the target URL (`session:navigate(url)`).
2. The bridge captures page state (`document:title()`, text or simplified DOM tree).
3. The bridge formats a structured prompt for OpenCode containing the extraction goals and JSON output schema.
4. The bridge calls `POST /session/:id/message` on OpenCode.
5. The received JSON output is validated against the schema and returned to the caller or saved to disk.

### Scenario 2: Autonomous Navigation & Next-Action Loop
For multi-step browsing:
1. The bridge exposes ProwseTk browser actions as OpenCode tool calls or commands (`click(selector)`, `fill(selector, value)`, `navigate(url)`, `evaluate(js)`).
2. The agent inspects the page state via the bridge, decides on the next action, and yields back tool calls.
3. The bridge executes the corresponding `prowsetk::Element` or `prowsetk::Session` native operations and returns results back to the agent session.

---

## Directory Structure
```text
plugins/opencode-bridge/
├── CMakeLists.txt          # Build configuration (compiles as shared library or static module)
├── include/
│   └── opencode_bridge.hpp # Public plugin declarations
├── src/
│   ├── bridge_core.cpp     # Plugin init, lifecycle, and config loading
│   ├── opencode_client.cpp # HTTP/SSE client implementation for OpenCode endpoints
│   ├── lua_bindings.cpp    # 'lopencode' Lua module definitions
│   └── dom_serializer.cpp  # DOM reduction and prompt preparation utilities
├── tests/
│   ├── test_client.cpp     # Unit tests against mock OpenCode HTTP server
│   └── test_lua_api.cpp    # Lua integration tests
└── README.md               # Configuration and usage instructions

`OpenCodeClient::prompt_message` is the additive synchronous text-parts protocol
available to opencode-marionette clients with an empty API prefix. Keep it distinct from `prompt`/`prompt_v2`,
retain transport budgets/redirection/authentication policy, reject assistant
errors, and ignore reasoning parts when extracting the assistant text.
