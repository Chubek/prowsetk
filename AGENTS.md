# AGENTS.md — ProwseTk Implementation Guide

This file is derived from `README.md` and is binding for every agent that
implements, extends, or reviews ProwseTk. Read `README.md` first; it is the
architecture of record. When this file and `README.md` disagree, `README.md`
wins and this file must be corrected.

## 0. MCP Tools

You must use the following MCP tools installed on this agent in development of ProwseTk:

- `prowsetk_oracle`
- `http_oracle`

Use `absyn_cache` to cache C/C++ files, and read back from them, instead of reading the files (unless they have been changed). Same goes for `python_absyn` with Python files.

## 1. Mission

ProwseTk is an embeddable C++ toolkit for programmable, headless web browsers.
It targets scraping, endpoint discovery, API automation, testing, and data
extraction. It does **not** wrap WebKit, Blink, or Gecko. Its engine is
**Flatworm**, a lightweight headless engine optimized for automation rather than
full browser compatibility or pixel-perfect rendering.

Maintain four execution layers with hard boundaries:

- **C++** — host application, engine core, native plugins (`ProwseTk-Plugin.h`).
- **Lua** — session control (`lprowse`) and extensions (`lprowsext`).
- **WASM** — portable, sandboxed plugins behind the `WasmRuntime` abstraction.
- **JavaScript** — page scripting only, executed by QuickJS inside Flatworm.

Do not blur these layers. JavaScript is not an extension mechanism. Lua must not
receive raw Wasmtime handles. The WASM runtime must stay behind `WasmRuntime`.

### Driver scripts

`drivers/` holds Lua driver scripts run by `prowsetk run <name>` (README
"Drivers"). A driver defines a global `main(args)` entrypoint returning an
integer exit code; `args` is a table of named arguments declared by
`[[drivers]]` in `Prowse.toml`, coerced to their declared TOML types by the
CLI. Secret arguments must never reach driver output or logs. Keep drivers
self-contained and network-independent where possible: each shipped driver
accepts an optional `html` argument that switches to `session:load_html` for
deterministic offline runs. New drivers ship with a hermetic integration test
in `tests/integration/test_drivers.cpp` and a `prowsetk run` CLI test in
`tests/integration/test_cli.cpp`.

## 2. Non-Negotiable Constraints

- Headless only: no display server, windowing system, GPU, or desktop
  environment may be required.
- No dependency on a third-party browser engine.
- The public C++ API is decomposed into `Browser`, `Session`, `Document`,
  `Element`, `NetworkClient`, `JavaScriptRuntime`, `LuaRuntime`, `WebPlatform`,
  `Storage`, and `EventDispatcher`. Keep those responsibilities separate.
- The native plugin ABI is C-compatible and versioned by
  `PROWSETK_PLUGIN_ABI_VERSION`. Do not leak C++ types across that boundary.
- WASM plugin contracts are defined in WIT. Do not design new plugins around raw
  C ABI functions such as `plugin->on_request(char*, char*)`.
- WASI is **off** by default. Network access is host-mediated through
  `NetworkClient`; plugins must not open arbitrary sockets.
- Secrets (cookies, authorization headers, API keys, session tokens, private form
  values) are redacted by default in logs and generated OpenAPI output.
- Endpoint discovery is heuristic. Never present inferred values as
  authoritative; preserve provenance and confidence.

## 3. Repository Layout

```text
prowsetk/
├── CMakeLists.txt
├── CMakePresets.json
├── README.md
├── AGENTS.md
├── cmake/
├── include/prowsetk/
├── src/
├── tests/
├── third_party/
├── wit/
├── lua/
├── plugins/
├── drivers/
├── examples/
├── resources/
└── scripts/
```

`third_party/` is populated from `.gitmodules` and is excluded by `.gitignore`.
`scripts/scaffold.sh` creates or refreshes this skeleton.

## 4. Build System Requirements

Create and maintain a CMake build at the repository root.

### 4.1 `CMakeLists.txt`

- `cmake_minimum_required(VERSION 3.25)` or newer. (Workflow presets require
  CMakePresets schema version 6, hence 3.25.)
- `project(ProwseTk VERSION <semver> LANGUAGES C CXX)`.
- Default to C++20; do not raise the standard without documenting it.
- Provide these options, all defaulting safely:
  - `PROWSETK_BUILD_TESTS` (default `ON`)
  - `PROWSETK_BUILD_EXAMPLES` (default `ON`)
  - `PROWSETK_BUILD_CLI` (default `ON`)
  - `PROWSETK_ENABLE_WASM` (default `OFF`)
  - `PROWSETK_WASM_RUNTIME` (default `wasmtime`)
  - `PROWSETK_ENABLE_WASI` (default `OFF`)
- `include(CTest)` and `enable_testing()` at top level, then
  `add_subdirectory(tests)`.
- Do not hardcode compiler flags into targets; apply them through helpers in
  `cmake/`.
- Optional dependencies must be optional. A build with WASM disabled must
  configure, build, and test with no WASM toolchain present.

### 4.2 `cmake/`

Put all reusable build logic here. At minimum:

- `ProwseTkHelpers.cmake` — target creation, namespaced aliases
  (`ProwseTk::core`), install/export rules.
- `CompilerWarnings.cmake` — one `prowsetk_set_warnings(<target>)` function, not
  per-target flag duplication.
- `Dependencies.cmake` — one place that locates or vendors each dependency and
  maps it to a `ProwseTk::` imported target.

Rules:

- Prefer `find_package(... CONFIG)`; fall back to `third_party/` sources.
- Never `file(GLOB ...)` to list sources; list them explicitly.
- Export a `ProwseTkConfig.cmake` / `ProwseTkTargets.cmake` package.
- Keep dependency wiring declarative; no switch forests spread across targets.

### 4.3 `CMakePresets.json`

Provide configure, build, and test presets that share names so
`cmake --preset X`, `cmake --build --preset X`, and `ctest --preset X` all work.
Use `${sourceDir}/build/${presetName}` as the binary dir and set
`CMAKE_EXPORT_COMPILE_COMMANDS=ON`.

Required presets:

- `default` — Release-with-debug-info dev build, tests on, examples on.
- `debug` — Debug build, tests on.
- `release` — optimized, tests off.
- `asan` — AddressSanitizer + UndefinedBehaviorSanitizer, tests on.
- `coverage` — coverage instrumentation, tests on.
- `wasm` — `PROWSETK_ENABLE_WASM=ON`, tests on.

Define matching `buildPresets` and `testPresets` for every configure preset. A
CMake workflow preset may reference only a single configure preset, so provide
three workflows and run all three in CI:

- `ci` — configure/build/test `default`.
- `ci-asan` — configure/build/test `asan`.
- `ci-wasm` — configure/build/test `wasm`.

Invoke them with `cmake --workflow --preset ci`, `cmake --workflow --preset
ci-asan`, and `cmake --workflow --preset ci-wasm`. Do not commit
`CMakeUserPresets.json`.

## 5. Test Requirements — CTest-Conformant Suites in `tests/`

All tests live under `tests/` and are registered with CTest. A test that cannot
be run by `ctest` does not exist.

### 5.1 Structure

```text
tests/
├── CMakeLists.txt          # includes CTest, adds subdirectories
├── unit/
│   ├── CMakeLists.txt
│   └── test_<component>.cpp
└── integration/
    ├── CMakeLists.txt
    └── test_<scenario>.cpp
```

- `tests/CMakeLists.txt` must call `include(CTest)`, `enable_testing()`, and
  `add_subdirectory(unit)` / `add_subdirectory(integration)`.
- Each component gets its own test translation unit and its own executable.

### 5.2 Registering tests

- Preferred: GoogleTest via `find_package(GTest)` or `FetchContent`, then
  `gtest_discover_tests(<target> PROPERTIES LABELS "<label>")`.
- Acceptable fallback when no framework is available: an executable that returns
  nonzero on failure, registered with
  `add_test(NAME <qualified-name> COMMAND <target>)`.
- Test names are deterministic, dotted, and stable, e.g.
  `prowsetk.unit.document.query_selector` and
  `prowsetk.integration.network.redirect`. Never rely on test ordering.

### 5.3 Properties every test must set

- `LABELS` — one or more of `unit`, `integration`, and a component label
  (`document`, `network`, `lua`, `javascript`, `wasm`, `plugin`,
  `endpoint-extraction`, `storage`, `web-interface`).
- `TIMEOUT` — a finite timeout on every test. No test may hang the suite.
- `WILL_FAIL` only for tests whose purpose is to assert failure.

### 5.4 Rules

- Tests are hermetic. No network, filesystem writes outside the test binary
  directory, or wall-clock sleeps by default.
- Tests that require the network must be labeled `network` and skipped unless
  `PROWSETK_ENABLE_NETWORK_TESTS=ON`.
- Tests that require WASM must be labeled `wasm` and skipped when
  `PROWSETK_ENABLE_WASM=OFF`.
- Every public C++ component and every Lua-facing API added or changed ships with
  unit coverage. Cross-layer behavior (e.g. Lua driving a session, a WASM plugin
  observing requests) gets an integration test.
- Redaction and capability-policy behavior must have explicit tests.
- Run the suite with `ctest --preset default`. A failing or flaky test blocks
  completion; do not delete or disable a test to make the suite pass.

## 6. Coding Rules

- C++20. RAII. No raw owning pointers. `std::filesystem` for paths.
- No `new`/`delete` in new code; use smart pointers or value types.
- No C++ exceptions across the plugin C ABI; convert to error codes at the
  boundary and document ownership.
- Prefer declarative data (tables, schemas, WIT, TOML) over hardcoded branch
  forests.
- Libraries in `third_party/` are vendored as-is. Do not patch them in-tree; put
  fixes in `cmake/` or upstream them.
- No secrets in source, logs, fixtures, or generated OpenAPI.
- Do not add production dependencies without adding them to `README.md`
  Dependencies, `.gitmodules`, and `cmake/Dependencies.cmake`.
- Keep `ProwseTk-Plugin.h` self-contained and includable from C.

## 7. Architecture Rules

- Separate semantics, legality, lowering, register/layout, and encoding concerns.
- Keep JavaScript execution isolated from Lua and from the host.
- The `WasmRuntime` interface is the only place Wasmtime is named.
- Storage is replaceable through C++ interfaces; no direct filesystem coupling
  in the engine.
- All cross-layer calls go through documented interfaces with explicit error
  propagation.
- Never embed machine-specific or engine-specific semantics into a higher layer.

## 8. Workflow

1. `bash scripts/scaffold.sh` — create or refresh the skeleton. The script uses
   `$PROWSETK_DIR`; when unset it targets the directory one level above itself.
2. `cmake --preset default`
3. `cmake --build --preset default`
4. `ctest --preset default`
5. `./scripts/verify-libs-installed.sh ...` when a dependency is in doubt.

New to the project? Create the skeleton first, then build the smallest vertical
slice: DOM parse → document query → test → Lua binding → test.

## 9. When We Know It Is Done

A change is done only when **all** of the following hold. If any item is false,
the work is not done.

- **Builds:** `cmake --preset default` configures and
  `cmake --build --preset default` compiles with no warnings added by the change.
- **Tests:** `ctest --preset default` passes, including every new test. New
  behavior has a registered, labeled, timeout-bounded CTest case.
- **Coverage:** every new or changed public C++ component and Lua-facing API has
  unit coverage; every new cross-layer path has an integration test.
- **No regressions:** `asan` preset passes. No previously passing test is
  disabled, deleted, or marked flaky.
- **Config matrix:** the `wasm` preset passes when WASM changes are involved, and
  a WASM-disabled build still configures, builds, and tests cleanly when it is
  not.
- **Contract:** public headers, `ProwseTk-Plugin.h`, and any WIT interface are
  updated; the C ABI stays C-compatible and remains at
  `PROWSETK_PLUGIN_ABI_VERSION` or is deliberately bumped.
- **Security:** no secret is logged, committed, or emitted; default redaction and
  capability policies still hold and are covered by tests.
- **Docs:** `README.md`, `AGENTS.md`, and the dependency table reflect the
  change. Discrepancies between code and docs are resolved, not deferred.
- **Hygiene:** no build artifacts, generated files, or vendored drops are added
  outside `third_party/`; `.gitignore` and `.gitmodules` are current.
- **Reproducibility:** a clean checkout plus
  `git submodule update --init --recursive` and `scripts/scaffold.sh` reproduces
  the build and the passing test suite.

## 10. Failure Modes To Avoid

- Adding a feature without a CTest case.
- Hanging tests, unbounded timeouts, or order-dependent suites.
- Exposing Wasmtime, raw pointers, or C++ types across the plugin or Lua
  boundaries.
- Enabling WASI or arbitrary sockets by default.
- Presenting inferred endpoints as authoritative API documentation.
- Patching `third_party/` in place.
- Duplicating build logic instead of using `cmake/` helpers.
- Letting `README.md`, `AGENTS.md`, `.gitmodules`, and `cmake/Dependencies.cmake`
  drift apart.

---

# Additions & Revisions

## The IR Emitters

Although ProwseTk is a headless browser and it does not render anything, the engine can generate an *intermediate representation* for the page. By default, ProwseTk supports four IRs, with more extensible via plugins. The Lua engine exposes `lprowseir` for handling IR pipelines:

- **ProwseXAS:** An event stream which can be listened to.
- **ProwseDOM:** A document object model which can be walked.
- **ProwseVTD:** A binary virtual token format which can be dumped.
- **ProwseIML:** An S-Expression language with macros which can be expanded.

Example of listening to an event stream in Lua:

```lua
lprowseir.xax:AddListener("//tag/td", function() ... end)
```

All listeners and walkers use XPath powered by Pugixml's XPath engine.

The implementation is stratified into five concerns with hard boundaries
(Architecture Rules §7), each in its own translation unit:

- **Semantics** (`src/core/ir_model.cpp`) — lowers the Flatworm DOM to the
  canonical event/node model. Knows nothing about XPath or encodings.
- **Register/layout** (`src/core/ir_layout.cpp`) — the sole owner of stable
  XPath-like paths, sibling indexes, and depths. Every IR consumes its
  assignments verbatim.
- **Legality** (`src/core/ir_filter.cpp`, `check_xpath_legality`) — validates
  XPath and selects subtrees. `lprowseir.dom.walk` and
  `lprowseir.xas:AddListener` fail fast on invalid expressions and propagate
  callback errors instead of swallowing them.
- **Lowering** (per emitter) — maps the canonical model to one IR shape.
  Never reparses HTML or touches the DOM directly.
- **Encoding** (`src/core/ir_vtd.cpp`, `src/core/ir_iml.cpp`) — VTD framing
  and IML text/macros. VTD decoding is strict and allocation-bounded, and
  attribute owner tags round-trip through the layout path.

Further IRs register by name with `IrEmitterRegistry`
(`src/core/ir_registry.cpp`; built-ins `"iml"`/`"vtd"`) instead of patching
the built-ins, and drivers resolve every name uniformly through
`lprowseir.emit(document, name)` / `lprowseir.emitters()`.

---

## The Flatworm Web Platform (Page JS Bindings)

Page JavaScript executes against the **web platform shim**
(`src/core/web_platform_shim.hpp`), a JavaScript bootstrap installed by the
QuickJS runtime over handle-based, host-mediated primitives
(`DocumentScriptHost` in `include/prowsetk/javascript_runtime.hpp`, implemented
by `FlatwormScriptHost` in `src/core/flatworm_host.hpp`, owned per `Session`).
It provides `document` (queries, traversal, mutation, `innerHTML`/`outerHTML`,
`classList`, `dataset`, `style`), element wrappers with listeners and
synthetic events, synchronous and asynchronous `XMLHttpRequest`, `fetch` with
`Headers`/`Response`, timers, `navigator`, `location` (assignment and link
clicks and `form.submit()` queue a bounded host navigation),
`localStorage`/`sessionStorage`, `document.cookie` through the session cookie
jar, `URL`/`URLSearchParams`, DOM events (capture, target, and bubble, including
`submit` from a submit control and `MutationObserver`), and inert stubs for
`IntersectionObserver` and `ResizeObserver`. `history.pushState` /
`replaceState` update `location` without a host navigation. Lifecycle flush
also drains microtasks queued by `DOMContentLoaded` and `load` handlers, so
`fetch` started there is host-mediated and can be recorded as an endpoint.

Rules:

- Page scripts never receive engine internals: DOM nodes cross the boundary
  only as opaque `ElementHandle` values owned by the host, invalidated when
  the document is reinstalled.
- Every script network call goes through the owning `Session` (`cookies`,
  redirects, `BeforeRequest`/`AfterResponse` events, plugins, and redaction
  apply); bindings never touch a socket directly.
- Lifecycle and timers run in bounded flush passes (`__prowsetkFlush`) after a
  document's scripts; host-mediated network time extends the script deadline
  and never consumes the JS budget.
- Support levels are declared honestly in `web_platform.cpp` and
  `quickjs_runtime.cpp` capabilities; do not present the shim as full
  browser compatibility (no layout, progress events, streaming bodies, or
  CORS enforcement).
- The plugin ABI and the Lua layer are unaffected: this is page scripting
  only (README "JavaScript Execution").

---

## Synthetic Interaction Driver & SPA Event Cascades

Flatworm does not have a layout tree, rasterizer, or hit-testing subsystem. Modern
Single Page Applications (React, Vue, Angular, Svelte) attach event listeners
globally to the document root and track controlled component values via native
descriptors. In order for synthetic actions executed by Lua drivers (`lprowse`) or
C++ host controllers to trigger state changes, form validations, and subsequent
network POST/PUT operations, Flatworm implements a dedicated **Synthetic
Interaction Driver** inside `web_platform_shim.hpp` and `FlatwormScriptHost`.

### 1. Pointer & Click Cascade Contract
Never dispatch an isolated `click` event. Calling `element.click()` or
`element:click()` must execute the standard browser event sequence in exact order:

1. `pointerover` (`bubbles: true`, `cancelable: true`, `composed: true`)
2. `pointerenter` (`bubbles: false`, `cancelable: false`)
3. `pointerdown` (`bubbles: true`, `cancelable: true`, `composed: true`)
4. `mousedown` (`bubbles: true`, `cancelable: true`, `composed: true`)
5. `focus` (if element is focusable and not already active)
6. `pointerup` (`bubbles: true`, `cancelable: true`, `composed: true`)
7. `mouseup` (`bubbles: true`, `cancelable: true`, `composed: true`)
8. `click` (`bubbles: true`, `cancelable: true`, `composed: true`)

If the target is a submit control (`<button type="submit">`, `<input type="submit">`,
or `<button>` within a `<form>`), the shim must trigger a bubbling `submit` event
on the enclosing `<form>` unless `event.preventDefault()` was invoked during the
click cascade.

### 2. Controlled Input & Keyboard Cascade (React / Framework Setters)
Direct assignment (`element.value = "..."`) modifies the DOM property without
triggering React fiber or Vue reactive trackers. Synthetic input interactions
(`element:type(text)` or `element:set_value(text)`) must invoke the native
prototype setter before firing the input stream:

```javascript
// Native descriptor bypass inside web_platform_shim.hpp
function __prowsetkSetValue(element, value) {
  const proto = Object.getPrototypeOf(element);
  const descriptor = Object.getOwnPropertyDescriptor(proto, 'value');
  if (descriptor && descriptor.set) {
    descriptor.set.call(element, value);
  } else {
    element.value = value;
  }
}
```

The keyboard/typing cascade must execute:
1. `focus`
2. For each character / chunk:
   - `keydown` (`key`, `code`, `bubbles: true`)
   - `keypress` (if printable)
   - Update value via `__prowsetkSetValue`
   - `input` (`bubbles: true`, `composed: true`, `inputType: 'insertText'`)
   - `keyup` (`key`, `code`, `bubbles: true`)
3. `change` (`bubbles: true`, `cancelable: false` on commit/blur)
4. `blur` (on focus lost)

### 3. Layout-Free Visibility & Interactability Heuristics
Because CSS layout is a stub, physical coordinate hit-testing is unavailable. The
shim and host determine element interactability through semantic DOM heuristics:

- **Visibility:** Evaluated via inline styles and attributes. An element is
  considered non-interactable if `style.display === 'none'`,
  `style.visibility === 'hidden'`, `hasAttribute('hidden')`, or if any ancestor
  matches these conditions.
- **Disabled:** An element is non-interactable if `disabled` or `aria-disabled="true"`.
- Interacting with an element that fails heuristic interactability must fail fast
  with a documented error code rather than silently dropping events.

### 4. Microtask & Network Draining Lifecycle
Every synthetic action dispatched via C++ (`Element::click`, `Element::type`) or
Lua (`elem:click()`, `elem:type()`) must conclude with a bounded call to
`__prowsetkFlush()`. This drains pending QuickJS promise jobs, timer queues,
and microtasks, ensuring network requests (`fetch`/`XHR`) triggered by the
framework reach the host `Session` and `NetworkClient` before the host action
returns.

---

## Booking.com example and HTTPS

`examples/booking-dotcom-admin-scrape/scrape-booking-dotcom-admin.lua` is an example driver
registered by `examples/booking-dotcom-admin-scrape/Prowse.toml`. It follows the `main(args)` and
offline `html` contracts despite living in `examples/`. Load dotenv before looking up its
Booking.com credentials; never log those values. Require positive login
evidence before exporting a live page. Try imported session cookies before
requiring credentials, follow bounded trusted HTTPS redirects for confirmation,
and crawl from the confirmed admin URL. Confirmation requires a successful HTTP
response and a DOM logout/account control, not words inside scripts. JavaScript
support is partial; human verification and MFA can still require browser
interaction followed by importing fresh cookies. Never claim those challenges
have been solved merely because a cookie exists.
Discovery is heuristic and bounded, with explicit incomplete coverage metadata.
After the crawl the driver applies restful-resolver and then schema-grabber
enrichment (request/response schemas, typed URL parameters; GET response
probes are same-origin and bounded, POST is never probed), so the exported
OpenAPI and Postman specs carry req, res, and URL parameters. api-only
garbage filtering (`--api-only`, on by default) drops endpoints that cannot
serve as proper API endpoints — static assets, bundles, plain pages — via
scrape-endpoints pattern lists before export.
JavaScript function expressions and arrow functions in endpoint paths are also
excluded from both exports, including recursively discovered paths; ordinary
query values do not trigger this path filter.
Tests cover the real extractor, simulated login flows, CLI, redaction,
schema enrichment, api-only filtering, and rejected form actions/redirects.

The POSIX transport optionally uses OpenSSL 3 for certificate-verified HTTPS.
Keep TLS dependency discovery in `cmake/Dependencies.cmake`, default trust and
hostname verification enabled, and the HTTP-only build usable without OpenSSL.
TLS tests use a local test CA and loopback peer; no public network is required.

---

## Assistant Browser Handoff

`Prowse.toml` may define `[assistant-browser]` (or `[assistant_browser]`) with
`enabled`, `command`, `method`, `endpoint`, `debug_port`, and
`wait_timeout_ms`. `$PROWSETK_ASSISTANT_BROWSER` overrides the command at
runtime. `scrape-endpoints` may offer this handoff when
heuristics detect anti-bot/human-verification content or when no endpoints are
found from the current document. The handoff is user-approved by default:
prompt on the CLI, launch the configured browser command with the page URL, wait
for the user to finish, then continue with the data explicitly available to the
session or plugin. Do not treat a successful handoff as authoritative proof of
coverage, and do not log or export cookies, tokens, form values, or challenge
content.

---

## Anti-bot detection and Captcha Handler

Core anti-bot detection is heuristic and reports provenance, confidence, and
signals through `AntiBotDetector`, `Session:detect_anti_bot`, and the
`anti_bot_detected` event. Do not present detections as authoritative proof.

`plugins/captcha-handler` builds on that subsystem and exposes host-mediated
handling plans only. It may prompt a user, call a configured solver API, dispatch
a webhook, invoke a Lua callback, reuse a pre-solved token, reuse a cookie
session, wait for clearance, or abort and report. Solver keys, cookies, tokens,
and form values remain secret-bearing inputs and must not be logged or emitted.

---

## RESTful resolver and Booking.com application

`plugins/restful-resolver` resolves scraped endpoints iteratively through the
owning `Session` until a full RESTful surface (at least one GET and at least
one POST) is discovered, or until `max_rounds`/`max_requests` is exhausted.
Seeds come from `EndpointExtractor`; each round issues host-mediated GET
probes, follows API-like JSON references, and re-parses HTML bodies so POST
forms and script POST calls surface with native methods and provenance. A POST
for an already-fetched URL counts without refetching; cross-origin URLs are
rejected unless `allow_cross_origin` is set from the current document. The handoff is user-approved by default:
prompt on the CLI, launch the configured browser command with the page URL, wait
for the user to finish, then continue with the data explicitly available to the
session or plugin. Do not treat a successful handoff as authoritative proof of
coverage, and do not log or export cookies, tokens, form values, or challenge
content.

---

## Anti-bot detection and Captcha Handler

Core anti-bot detection is heuristic and reports provenance, confidence, and
signals through `AntiBotDetector`, `Session:detect_anti_bot`, and the
`anti_bot_detected` event. Do not present detections as authoritative proof.

`plugins/captcha-handler` builds on that subsystem and exposes host-mediated
handling plans only. It may prompt a user, call a configured solver API, dispatch
a webhook, invoke a Lua callback, reuse a pre-solved token, reuse a cookie
session, wait for clearance, or abort and report. Solver keys, cookies, tokens,
and form values remain secret-bearing inputs and must not be logged or emitted.

---

## RESTful resolver and Booking.com application

`plugins/restful-resolver` resolves scraped endpoints iteratively through the
owning `Session` until a full RESTful surface (at least one GET and at least
one POST) is discovered, or until `max_rounds`/`max_requests` is exhausted.
Seeds come from `EndpointExtractor`; each round issues host-mediated GET
probes, follows API-like JSON references, and re-parses HTML bodies so POST
forms and script POST calls surface with native methods and provenance. A POST
for an already-fetched URL counts without refetching; cross-origin URLs are
rejected unless `allow_cross_origin` is set. Output is deterministic OpenAPI
3.x YAML with `x-prowsetk-restful` (`has-get`, `has-post`, `is-complete`,
`rounds-used`, `request-count`), provenance, confidence, and redaction.
Discovery is heuristic, never authoritative.

`examples/booking-dotcom-admin-scrape` registers the plugin in its
`Prowse.toml` and applies it after the crawl via
`lua/restful_resolver.lua`: `resolve_until

---

## Schema grabber and scrape-endpoints composition

`plugins/schema-grabber` reverse-engineers scraped endpoints into full
request/response schemas plus URL parameters and composes with
`plugins/scrape-endpoints` (scrape for discovery, grabber for enrichment).
Query parameters are typed from example values (`boolean`/`integer`/`number`/
`string`, always `required: false`); volatile path segments (numeric ids,
UUIDs, long hashes) are templated (`/api/users/123` → `/api/users/{id}`)
with the original kept as the example. POST/PUT/PATCH request schemas prefer
matching form fields (input types mapped, `required` honored), then JSON body
hints near the endpoint path in inline scripts, then a generic inferred
object when a body content type is known; GET endpoints get no request body.
Response schemas come from observed bytes only: bounded host-mediated GET
probes for GET endpoints through the owning `Session`
(`probe_get_responses`/`max_probe_requests`, same-origin unless
`allow_cross_origin`), or `ResolvedBody` bytes resolved alongside
`scrape-endpoints` via `enrich_endpoints`. POST/PUT/PATCH endpoints are never
probed with their own method. Output is deterministic OpenAPI 3.x YAML with
`x-prowsetk-schema` (request/response provenance) and Postman 2.1 JSON with
query pairs, `:var` path variables, and example bodies. Inference is
heuristic, never authoritative; sensitive names/values keep their type but
lose their example to redaction. The native ABI stays at
`PROWSETK_PLUGIN_ABI_VERSION`; the WIT contract for a future WASM component
is `wit/schema-grabber.wit`; the Lua spec layer is
`lua/schema_grabber.lua` (`enrich(session, spec)`).

---

## Beacon Firefox handoff

`plugins/beacon` supplies a native Flash plugin, an owner-only local Unix
socket broker (`beacond`), a framed Firefox Native Messaging host, a driver-side
`beaconctl`, and a Firefox addon. Drivers register a Flash through the local
client and poll its bounded delivery queue. The addon lists seeking Flashes,
requires an explicit user click to connect to a matching HTTP(S) tab, and
sends data only from that tab. Tab navigation and closure revoke the addon
connection. Page HTML and styles may contain secrets and are delivered verbatim
after consent; network metadata omits queries and request bodies. Treat the
socket owner as trusted: no token authentication is implemented in the broker,
and the addon click cannot protect against another same-user socket client.
Tracepoints currently observe CSS-selector-scoped DOM mutations; full HAR,
request tracepoints and stylesheet tracepoints are not implemented. Keep the
broker integration and addon simulation CTests registered with bounded timeouts.
See `plugins/beacon/README.md` for protocol and installation details.
