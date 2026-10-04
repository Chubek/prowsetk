z AGENTS.md — ProwseTk Implementation Guide

This file is derived from `README.md` and is binding for every agent that
implements, extends, or reviews ProwseTk. Read `README.md` first; it is the
architecture of record. When this file and `README.md` disagree, `README.md`
wins and this file must be corrected.

You must use the following MCP tools installed on this agent in development of ProwseTk:

- `prowsetk_oracle`
- `http_oracle`

## 1. Mission

ProwseTk is an embeddable C++ toolkit for programmable, headless web browsers.
It targets scraping, endpoint discovery, API automation, testing, and data
extraction. It does **not** wrap WebKit, Blink, or Gecko. Its engine is
**Flatworm**, a lightweight headless engine optimized for automation rather than
full browser compatibility or pixel-perfect rendering.

Maintain four execution layers with hard boundaries:

- **C++** — host application, engine core, native plugins (`ProwseTk-Plugin.h`)
  and native page-runtime modules (`Flatwork-Module.h`).
- **Lua** — session control (`lprowse`) and extensions (`lprowsext`).
- **WASM** — portable, sandboxed plugins behind the `WasmRuntime` abstraction.
- **JavaScript** — page scripting only, executed by QuickJS inside Flatworm.

Do not blur these layers. Page JavaScript does not implement browser extensions;
native Flatworm modules extend its runtime from C/C++. Lua must not receive raw
Wasmtime handles. The WASM runtime must stay behind `WasmRuntime`.

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
- The independent native Flatworm module ABI is C-compatible and versioned by
  `FLATWORM_MODULE_ABI_VERSION` in `include/Flatwork-Module.h`. Do not expose
  QuickJS handles, DOM pointers or Lua state across it.
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
├── flatworm-modules/
├── drivers/
├── examples/
├── resources/
├── manual/
└── scripts/
```

`third_party/` is populated from `.gitmodules` and is excluded by `.gitignore`.
`scripts/scaffold.sh` creates or refreshes this skeleton.

`manual/README.md` indexes 30 separate numbered Markdown chapters covering the
implemented engine, APIs, Lua extensions, plugins and tools. Keep examples and
support levels aligned with callable interfaces when extending those surfaces.
`scripts/build-docs.sh` checks chapter completeness and builds combined HTML and
LaTeX through Pandoc, using `scripts/manual-links.lua` for navigation links.

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

## AI oracle

`plugins/ai-oracle` uses OpenAIpp's JSON/authentication helpers behind the host
`NetworkClient` boundary. Keep dependency discovery in
`cmake/Dependencies.cmake`; missing optional headers or
`PROWSETK_BUILD_AI_ORACLE=OFF` must preserve the rest of the build. Inquiries are
explicit per-client calls through `Oracle`, the opaque C service, or `ai_oracle`
Lua userdata. Loading the ABI-v2 native facade must remain network-free.

Preserve disabled defaults, HTTPS-only explicit API bases, redirect rejection,
bounded input/response/JSON depth/token/attempt budgets, detached DOM sanitization,
recursive structured redaction, and secret-free errors. Embedded images are
caller-supplied PNG/JPEG data URLs, transmitted verbatim within the input budget;
never fetch arbitrary image URLs or claim pixel redaction. Responses remain
advisory with provenance; CAPTCHA clearance, authentication, browser actions and
crawler policy stay with the calling plugin/driver. Never execute model output
as Lua, JavaScript, shell commands, or native tools. Lua transport callbacks may
not close or reenter the same active client. Keep unit and integration coverage
under `tests/`, using hermetic host transports rather than live OpenAI calls.
Managed Lua session/extractor finalizers must release their shared lifetime
guards after invalidating callbacks; retain GC and leak-enabled coverage. A
Lua-owned `Browser` must outlive its sessions: `browser_gc` defers the delete
until `Browser::live_session_count()` reaches zero, so never delete an owned
browser directly or restore a session's raw browser pointer to a shared owner.

## Persistent spider plugin

`plugins/spider` builds `ptkspiderd`, `ptkspiderctl`, and the version-2 native
plugin on Linux when LMDB and `third_party/lmdbxx` are available. Each named
worker owns its Browser/Session and private LMDB environment; crawler and
driver network requests, redirects and page-script requests stay host-mediated
and same-origin. Keep the daemon's nonblocking IPC loop separate from worker
browser execution. Never execute controller arguments through a shell.

Preserve bounded durable breadth-first frontiers, robots-aware automatic GET
visits, bounded retries, periodic watch epochs, cache retention, partial-client
timeouts, process watchdogs and paused restart recovery. The current frontier
task is removed only with a committed page/state transition. Driver runtime
state and session credentials are process-local; do not claim they survive a
restart. The last sanitized snapshot, frontier and watch schedule do survive.

`lspider` uses managed `lprowse` session handles. Driver cache/frontier writes
are staged until main(args) returns 0; browser actions are immediate. Drivers
are trusted automation, not a sandbox. Cache/control output removes private
form values, script content, sensitive attributes and query parameters by
default, and never emits raw cookies/headers or detailed driver errors.
Owner-only cache/socket permissions remain required. Keep new tests under
`tests/unit/test_spider.cpp` and `tests/integration/test_spider_daemon.py`,
registered with labels and finite timeouts. See `plugins/spider/README.md`
for the exact supported command, robots and recovery contracts.

## The IR Emitters

Although ProwseTk is a headless browser and it does not render anything, the engine can generate an *intermediate representation* for the page. Its canonical in-memory IR is the ProwseEvent stream; the Lua engine exposes `lprowseir` for handling it and its wire encodings:

- **ProwseEvent:** A start/attribute/text/end stream. Start records carry the
  former flattened-node attributes and subtree text, so no parallel DOM IR is
  maintained.
- **ProwseXAS/ProwseDOM:** Compatibility projections over ProwseEvent for
  existing callers; do not add new semantics to them.
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
  canonical ProwseEvent model. Knows nothing about XPath or encodings.
- **Register/layout** (`src/core/ir_layout.cpp`) — the sole owner of stable
  XPath-like paths, sibling indexes, and depths. Every IR consumes its
  assignments verbatim.
- **Legality** (`src/core/ir_filter.cpp`, `check_xpath_legality`) — validates
  XPath and selects subtrees. `lprowseir.dom.walk` and
  `lprowseir.xas:AddListener` fail fast on invalid expressions and propagate
  callback errors instead of swallowing them.
- **Lowering** (per emitter) — maps the canonical model to one IR shape.
  Never reparses HTML or touches the DOM directly.
- **Encoding** (`src/core/ir_events.cpp`, `src/core/ir_vtd.cpp`,
  `src/core/ir_iml.cpp`) — NDJSON event transport, VTD framing, and IML
  text/macros. VTD decoding is strict and allocation-bounded, and
  attribute owner tags round-trip through the layout path.

Further IRs register by name with `IrEmitterRegistry`
(`src/core/ir_registry.cpp`; built-ins `"events"`/`"iml"`/`"vtd"`) instead of patching
the built-ins, and drivers resolve every name uniformly through
`lprowseir.emit(document, name)` / `lprowseir.emitters()`.

`prowsetk serialize` emits ProwseEvent NDJSON, serialized ProwseVTD, or ProwseIML from a
fresh CLI session. `tools/page2pdf` is a C-only consumer of all three encodings
and accepts `-` as standard input for pipelines.
All encodings replay into the same start/attribute/text/end renderer sink;
keep CLI pipeline coverage for every format and enforce the shared 512-page
output bound.
It must remain downstream of the IR boundary: it never accesses Flatworm DOM
objects or the C++ API. It builds a bounded print box layout from IR attributes
and embedded styles, then paints backgrounds, borders, text, and embedded images
with libHaru. The CLI may snapshot bounded same-origin CSS and PNG/JPEG via the
owning session into serialized IR. Neither component claims browser/CSS layout
compatibility beyond the documented supported subset.

`tools/page2latex` is a separate downstream consumer for canonical ProwseEvent
NDJSON only. It must remain independent of Flatworm and the C++ API, parse the
bounded start/attribute/text/end stream strictly, and escape all page text for
LaTeX. `--hyperref` is opt-in; templates require a `{{PAGE2LATEX_BODY}}`
marker. `.pdf` output or `--make-pdf` invokes the explicitly configured
`$PWTK_PAGE2LATEX_ENGINE` (default `xelatex`); LaTeX engine failures must be
propagated rather than yielding a blank or partial PDF.

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

### Native Flatworm page-runtime modules

Keep definition validation, shared-library ownership and host registration in
`src/flatworm/module.cpp`; QuickJS lowering and the installed-only `flatworm:`
import loader belong in `src/flatworm/module_bindings.cpp`. The public C++
loading/registry interface is `flatworm_module.hpp`, and `JavaScriptRuntime`
owns installation and ECMAScript module evaluation. Browser module registrations
are inherited by future runtimes/sessions. Capability queries must not create
native instances. Modules are explicit, trusted native host code with
synchronous callbacks; JavaScript time/memory limits cannot sandbox them.

Preserve frozen export namespaces, typed scalar/length-delimited string/JSON
transfers, count/byte/call-depth bounds, generic value-free callback errors,
atomic failed installation, cleanup after failed initialization, reverse-order
shutdown and library retention through context/job destruction. Module state
persists with the session's JavaScript context across document loads and is
released on session closure. Native callbacks must not reenter the same runtime
or open sockets for pages; networking remains host-mediated. Static module
script elements support native imports only; do not claim general remote module
graphs or dynamic DOM-injected module scripts. QuickJS-disabled runtimes report
Unsupported and JavaScript-disabled sessions create no native instances.

Keep C ABI/C++ entry-linkage shared-library fixtures and labeled, timeout-bounded
coverage under `tests/modules`, `tests/unit/test_flatworm_modules.cpp` and
`tests/integration/test_flatworm_modules.cpp`, including ABI validation,
ownership, native exceptions, transfer bounds, import policy, lifecycle and
cross-layer behavior. The buildable C module and embedder example lives in
`examples/flatworm-module`; modules need only the ABI header to compile.

The first shipped native module is `flatworm-modules/rpc` (`flatworm:rpc`),
built with `PROWSETK_BUILD_FLATWORM_MODULES=ON`. Keep JSON syntax, encoding,
protocol validation/correlation and ABI adaptation separate. It uses only the
C++20 standard library and the module ABI; networking remains page fetch/XHR
through the owning Session. Preserve per-runtime automatic IDs, notification
no-reply rules, type-sensitive ID correlation, exact safe-integer validation
before wire-number rounding, duplicate-key rejection, UTF-8 validation and
bounded JSON/batches/output. Payloads remain caller data; errors/logs never
include their values, including getter/toJSON conversion failures. Session
events must propagate cancellation back to the request/navigation path.
Keep hermetic, labeled, timeout-bounded unit/integration coverage in
`tests/unit/test_rpc_module.cpp` and `tests/integration/test_rpc_module.cpp`.
See the module README for its exports and supported protocol restrictions.

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
response and a DOM logout/account control or an explicitly configured success
beacon, not words inside scripts. JavaScript
support is partial; human verification and MFA can still require browser
interaction followed by importing fresh cookies. Never claim those challenges
have been solved merely because a cookie exists.
`scripts/run-scrape-booking.sh` defaults to a forced, user-approved assistant
browser handoff, the XPath success beacon
`xpath=//h1[contains(normalize-space(.), 'Joe Litty Rooms')]`, and `--no-xcors`
for booking.com TLD-only endpoint output. Keep these launcher defaults
overridable and forward explicit boolean values to the driver CLI.
Launch the Booking assistant browser independently of the scraper's terminal;
show the Enter prompt while the browser is still open. EOF cancels the handoff,
and resuming still requires fresh session data and positive DOM confirmation.
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
signals through `AntiBotDetector`, C++ `Session::anti_bot_detection()`, and the
`anti_bot_detected` event. Lua subscribes to that event or uses captcha-handler's
response helper; its generic Session method table has no `detect_anti_bot()`.
Do not present detections as authoritative proof.

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
signals through `AntiBotDetector`, C++ `Session::anti_bot_detection()`, and the
`anti_bot_detected` event. Lua subscribes to that event or uses captcha-handler's
response helper; its generic Session method table has no `detect_anti_bot()`.
Do not present detections as authoritative proof.

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


## Flatworm selector layers

Keep selector syntax data in `src/flatworm/selector_model.hpp`, parsing and
escape decoding in `selector_parser.cpp`, DOM matching in `selector_matcher.cpp`,
and iterative query traversal in `css_selector.cpp`. All callers (C++, Lua,
and page JavaScript) share these semantics. Preserve the documented syntax and
matching limits, explicit errors for unsupported selectors, element receiver
exclusion, and document-order deduplication. Extend unit and cross-layer tests
when selector semantics change; do not claim full CSS conformance.


## Flatworm HTML layers

Flatworm HTML processing separates value-only syntax records (`html_model.hpp`),
streaming tokenization (`html_tokenizer.cpp`),
character references (`html_entities.cpp`), shared syntax vocabulary
(`html_syntax.cpp`), tree construction (`html_parser.cpp`), node ownership and
mutation (`dom_node.cpp`), and HTML encoding (`html_serializer.cpp`). The
same parser serves document loads and DOM fragments across C++, Lua, and page
JavaScript. It keeps the first duplicate attribute, accepts punctuation in
attribute names, requires a delimited matching raw-text/RCDATA end tag, and
ignores the trailing solidus on non-void HTML start tags. Numeric references
support optional semicolons, invalid-scalar replacement, and HTML C1 remapping;
named references remain restricted to the documented source vocabulary with
required semicolons. Scoped omitted-end-tag recovery covers paragraphs, list
items, definition items, options/groups, and table cells/rows/sections.

Parsing fails with `ErrorCode::ResourceLimit` above 16 MiB of input, 250,000
total nodes (including the document node), or 256 node levels below the document.
These bounds apply to each parse, including fragments; they are not a cumulative
quota on later DOM mutations. The parser remains a restricted tolerant HTML
parser: no implicit html/head/body or table wrappers, foster parenting,
adoption-agency reconstruction, foreign-content namespaces, full named-entity
vocabulary, input encoding sniffing, or context-sensitive fragment insertion
modes. This expansion does not imply full HTML5 conformance.

Keep tokenization independent of node allocation and tree semantics; keep
serialization and mutation out of the parser. Test recovery, malformed inputs,
resource bounds, and C++/Lua/page-JavaScript agreement when changing this path.

### Synthetic interaction completion

Host-mediated synthetic clicks and typing drain bounded lifecycle work and
queued navigation before returning. Form-control `value` prototype descriptors
are configurable so framework setter wrappers can observe the native setter
path. Repeated clicks on an already-active element omit the focus event.
A rejected HTML parse preserves the previously installed document and session URL.

### Python build isolation

Python binding builds keep their module, package wrapper, and generated stubs
inside each preset's binary directory. CTest imports that package. Sanitizer
runtime discovery lives in `cmake/PythonSanitizers.cmake`; runtime preloading
and Python-only leak suppression apply to stub generation and pytest, while
C++ sanitizer tests retain their configured leak checking. Clang compiler-rt
discovery uses the compiler's target triple. The ASan test preset accepts
equal-sized duplicate globals from the static core linked into native plugins
while still checking size-mismatched ODR violations. Preset-local packages prevent
default and ASan builds from overwriting each other's Python modules in the
source tree.

Sanitized Debug builds compile QuickJS with `-O1` to keep instrumentation-induced
native-stack growth within its existing stack bound; sanitizers and debug
information remain enabled.

Encrypted-storage tests retain production-cost key derivation and use a bounded
300-second timeout to accommodate sanitizer instrumentation; other unit tests
retain the default 60-second bound.

## DOM and DOM Maniplators

### Implemented PDQL and automation tools

`tools/crawler` is the POSIX Lua/TOML crawler host. Keep crawl scheduling in
`lua/lcrawler.lua`, reuse the ezlogin and scrape-endpoints Lua plugin APIs, and
enforce origin/request bounds at the host transport boundary (including
redirects and page scripts). Preserve robots checks, bounded breadth-first
frontiers, offline HTML mode, positive DOM login evidence, default redaction
and explicit incomplete-coverage summaries. Assistant projects/browser commands
require user approval and bounded argv-based process execution; installed
assistant projects resolve `prowsetk` beside explicit crawler paths or on `PATH`.
Successful handoffs still require fresh session data and login confirmation. The
Booking.com configuration lives in `tools/crawler/booking-dotcom-admin`.

`tools/pagewatch` is Linux-only. `pgwatchd` keeps nonblocking control IPC
separate from each Lua/Flatworm worker and from configured Lua actions.
`pgwatchctl` copies watcher scripts/configurations into owner-only deployment
directories under `/var/run/pagewatch` by default. `lpgwatch` queries managed
sessions with core `lpdql`; compare bounded, sanitized projections, not raw
page credentials. Preserve process watchdogs (including coroutine loops), worker
termination on abrupt daemon death, partial-client timeouts, same-UID socket
policy, bounded queues, explicit lost
history and paused restart recovery. Browser/Lua state, pending actions and
event history do not persist; `/var/run` does not imply reboot persistence.

The implemented PDQL slice is bounded tag globs, core XPath, projections,
equality guards, trimming and numeric aggregates, with JSON/YAML/XML/S-expression
serialization. `lpdql.query`/`rows`/`validate` and the C/C++ APIs share it.
Unsupported language features fail explicitly; the specification below is a
design direction, not a claim of full marionette/RE2 support. PDQL HTML input
uses Flatworm's bounded parser rather than a second HTML tokenizer. New live
DOM querying has default snapshot redaction and does not mutate the page.

Keep unit coverage in `tests/unit/test_pdql.cpp` / `test_pagewatch.cpp`, crawler
integration in `tests/integration/test_crawler.cpp` / `test_crawler_cli.py`,
and daemon/control integration in `test_pagewatch_daemon.py`. Driver adapters
also retain `test_drivers.cpp` and `test_cli.cpp` coverage. See the tool READMEs
for the exact supported contracts.

This is an addition to ProwseTk that will allow further uses of it. What you will add 
to ProwseTk's core engine is a *queryable DOM*. At the moment, the DOM is weak. What
you will add is a DOM that can be queried, manipulated, and serialized. 

In a browser, the DOM is part of the rendering engine. At the moment, ProwseTk uses 
event-based IRs which handle the task of rendering to middlewares. I want you to
create a DOM which is not about rendering, rather, it's about querying the page via
XPath, and a query language knowin as "PDQL" or "ProwseTk DOM Query Language".
We provide the Lua drivers with `lpdql` library, which allows Lua drivers to
query the DOM and serialize it. We also add a command in the CLI for making
a PDQL query. **We also expose PDQL in the ProwseTk C/C++ API**.

PDQL is a declarative language with basic computational facilities. Also, 
PDQL has the feature to let loose "marionattes" into the DOM, and serialize
the result of the query to JSON, YAML, XML and S-Expressions. PDQL can be mixed
with XPath. We can also use regular expressions in PDQL (regex using `third_party/re2`).
Another thing to note is that, HTML tags in PDQL are delimited by angle brackets, e.g.
`<h1>`. Glob patterns apply, e.g. `<h*>` means all heading tags. Glob patterns
can be used all over PDQL.

The architecture of it is depitcted below:
```
                      +-----------------------------------+
                      |      HTML / Event IR Source       |
                      +-----------------+-----------------+
                                        | (Parse / Ingest)
                                        v
                      +-----------------------------------+
                      |      ProwseTk Queryable DOM       |
                      |  (Tree, Attributes, XPath Index)  |
                      +-----------------+-----------------+
                                        |
                 +----------------------+----------------------+
                 |                                             |
                 v                                             v
       +--------------------+                        +--------------------+
       |   PDQL Engine      |                        |  RE2 & XPath       |
       |  (AST, Marionette  | <--------------------> |  Engines           |
       |   Traversals)      |                        +--------------------+
       +---------+----------+
                 |
     +-----------+-----------+-------------------+
     |                       |                   |
     v                       v                   v
+-------------+      +---------------+   +---------------+
| C/C++ API   |      |  Lua (lpdql)  |   |  CLI Driver   |
| prowsetk.h  |      |   Bindings    |   | prowsetk-cli  |
+-------------+      +---------------+   +---------------+
     |
     v Serializers
+-------------------------------------------------------+
|        JSON   |   YAML   |   XML   |   S-Expressions  |
+-------------------------------------------------------+
```

## 1. PDQL Language Specification & Syntax

PDQL is a declarative query language built around DOM structural patterns, tag globs (`<h*>`), XPath mixins (`xpath(...)`), regular expressions (`re2`), and **Marionettes** (autonomous micro-traversals that crawl child contexts, perform local computations, and emit projection maps).

### Key Features
1. **Delimited Tags & Globs:** `<h*>`, `<div*>` match element nodes matching standard glob patterns.
2. **XPath Integration:** `xpath("//main//article")` (or `$(//main//article)`) or inline predicates `[@data-type="post"]`. **XPath is provided by `third_party/pugixml`**.
3. **RE2 Integration:** `rx"pattern"` can match attributes, inner text, or node content.
4. **Basic Computational Facilities:** `count()`, `sum()`, `avg()`, string transformations, and boolean condition guards.
5. **Marionettes:** Sub-query walkers declared with `marionette { ... }` that walk nested subtrees and extract structured records.
6. **Serialization Targets:** Output targets specified with `serialize as [json | yaml | xml | sexpr]`.

---

## 2. PDQL Syntax Examples

### Example 1: Basic Tag Globbing with RE2 Text Filter
Extract all heading levels (`<h1>` through `<h6>`) where the text mentions "Engine" or "DOM", outputting as JSON:

```pdql
query HeadingsQuery {
    from <h*>
    where text matches rx"^(?i).*(engine|dom).*"
    select {
        tag: node.tag_name,
        level: node.tag_name.replace("h", ""),
        title: node.text.trim()
    }
    serialize as json
}
```

---

### Example 2: Mixing PDQL with XPath and Computations
Select cards inside a catalog, perform arithmetic on scraped values, and output as YAML:

```pdql
query ProductCatalog {
    from xpath("//div[contains(@class, 'product-card')]")
    where number(node.attr("data-price")) > 100
    select {
        sku: node.attr("data-sku"),
        title: find(<h3*>) -> first().text,
        base_price: number(node.attr("data-price")),
        tax: number(node.attr("data-price")) * 0.09,
        final_price: number(node.attr("data-price")) * 1.09
    }
    serialize as yaml
}
```

---

### Example 3: Deep Traversal Using Marionettes
A Marionette walks deep into an article node, recursively collects comments, author metadata, and linked resources, serializing to S-Expressions:

```pdql
query ForumThread {
    from <article*>
    where node.attr("id") matches rx"^thread-[0-9]+"
    select {
        thread_id: node.attr("id"),
        header: find(<h1*>) -> text,
        
        -- Marionette crawling thread comments
        comments: marionette {
            crawl <div*> where class matches rx".*comment-box.*"
            select {
                author: find(<span* class="author">) -> text,
                karma: number(find(<span* class="karma">) -> text.fallback("0")),
                body: find(<p*>) -> text,
                mentions: find(<a*>) -> filter(rx"^@\w+") -> collect(text)
            }
        },
        
        total_comments: count(comments)
    }
    serialize as sexpr
}
```

**Corresponding S-Expression Output:**
```lisp
((thread_id "thread-4021")
 (header "ProwseTk 2.0 Architectural Update")
 (comments
   (((author "alice") (karma 14) (body "Great approach.") (mentions ("@bob")))
    ((author "bob") (karma 8) (body "XPath + RE2 is fast.") (mentions ()))))
 (total_comments 2))
```

---

## 3. C / C++ Engine Header (`include/prowsetk/pdql.h`)

This exposes the queryable DOM, RE2 integration, PDQL parser/evaluator, and serialization formats to C and C++ consumers.

```c
#ifndef PROWSETK_PDQL_H
#define PROWSETK_PDQL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdbool.h>

/* Opaque Handles */
typedef struct pt_dom_document pt_dom_document_t;
typedef struct pt_dom_node     pt_dom_node_t;
typedef struct pt_pdql_query   pt_pdql_query_t;
typedef struct pt_pdql_result  pt_pdql_result_t;

typedef enum {
    PT_SERIALIZE_JSON = 0,
    PT_SERIALIZE_YAML,
    PT_SERIALIZE_XML,
    PT_SERIALIZE_SEXPR
} pt_serialize_format_t;

/* --- DOM Construction & Querying --- */
pt_dom_document_t* pt_dom_create_from_html(const char* html, size_t len);
void               pt_dom_free(pt_dom_document_t* doc);

/* Node manipulations */
const char*        pt_dom_node_tag(const pt_dom_node_t* node);
const char*        pt_dom_node_attr(const pt_dom_node_t* node, const char* attr_name);
const char*        pt_dom_node_text(const pt_dom_node_t* node);
pt_dom_node_t*     pt_dom_node_parent(const pt_dom_node_t* node);

/* --- PDQL Engine --- */
pt_pdql_query_t*   pt_pdql_compile(const char* pdql_src, char** error_out);
void               pt_pdql_query_free(pt_pdql_query_t* query);

pt_pdql_result_t*  pt_pdql_execute(pt_pdql_query_t* query, pt_dom_document_t* doc);
void               pt_pdql_result_free(pt_pdql_result_t* res);

/* --- Serialization --- */
char*              pt_pdql_serialize(pt_pdql_result_t* res, pt_serialize_format_t fmt);
void               pt_pdql_str_free(char* s);

#ifdef __cplusplus
}
#endif

#endif /* PROWSETK_PDQL_H */
```

---

## 4. Lua Driver Bindings (`lpdql`)

Drivers written in Lua can query documents, manipulate node subtrees, and retrieve results directly as native Lua tables or formatted strings.

### Lua C-Module Registration (`lpdql.c`)
```c
#include <lua.h>
#include <lauxlib.h>
#include "prowsetk/pdql.h"

static int lpdql_query(lua_State* L) {
    pt_dom_document_t** udoc = (pt_dom_document_t**)luaL_checkudata(L, 1, "ProwseTk.DOM");
    const char* query_str = luaL_checkstring(L, 2);
    const char* format_str = luaL_optstring(L, 3, "json");

    char* err = NULL;
    pt_pdql_query_t* q = pt_pdql_compile(query_str, &err);
    if (!q) {
        lua_pushnil(L);
        lua_pushstring(L, err ? err : "PDQL parse error");
        return 2;
    }

    pt_pdql_result_t* res = pt_pdql_execute(q, *udoc);
    
    pt_serialize_format_t fmt = PT_SERIALIZE_JSON;
    if (strcmp(format_str, "yaml") == 0) fmt = PT_SERIALIZE_YAML;
    else if (strcmp(format_str, "xml") == 0) fmt = PT_SERIALIZE_XML;
    else if (strcmp(format_str, "sexpr") == 0) fmt = PT_SERIALIZE_SEXPR;

    char* serialized = pt_pdql_serialize(res, fmt);
    lua_pushstring(L, serialized);

    pt_pdql_str_free(serialized);
    pt_pdql_result_free(res);
    pt_pdql_query_free(q);
    return 1;
}

int luaopen_lpdql(lua_State* L) {
    static const struct luaL_Reg lpdql_funcs[] = {
        {"query", lpdql_query},
        {NULL, NULL}
    };
    luaL_newlib(L, lpdql_funcs);
    return 1;
}
```

### Usage in Lua Driver Script:
```lua
local lpdql = require("lpdql")

function handle_page_driver(dom)
    local query = [[
        query ScrapeArticles {
            from <article*>
            select {
                title: find(<h1*>) -> text,
                author: find(<span* class="author">) -> text,
                link: node.attr("data-url")
            }
            serialize as json
        }
    ]]

    local json_output, err = lpdql.query(dom, query, "json")
    if err then
        print("Driver PDQL Error: " .. err)
        return
    end

    print("Scraped payload: " .. json_output)
end
```

---

## 5. CLI Command Implementation

Adds a `pdql` sub-command to the ProwseTk CLI tool:

```bash
prowsetk pdql --query "query { from <h*> select { t: node.text } serialize as json }" --file page.html
prowsetk pdql -q "from xpath('//table') select { rows: count(<tr*>) }" -s yaml -i page.html
```

### CLI Implementation (`src/cli/cmd_pdql.c`):
```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "prowsetk/pdql.h"

int cmd_pdql(int argc, char** argv) {
    const char* file_path = NULL;
    const char* query_src = NULL;
    pt_serialize_format_t fmt = PT_SERIALIZE_JSON;

    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--query") == 0) && i + 1 < argc) {
            query_src = argv[++i];
        } else if ((strcmp(argv[i], "-i") == 0 || strcmp(argv[i], "--file") == 0) && i + 1 < argc) {
            file_path = argv[++i];
        } else if ((strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--serialize") == 0) && i + 1 < argc) {
            const char* s = argv[++i];
            if (strcmp(s, "yaml") == 0) fmt = PT_SERIALIZE_YAML;
            else if (strcmp(s, "xml") == 0) fmt = PT_SERIALIZE_XML;
            else if (strcmp(s, "sexpr") == 0) fmt = PT_SERIALIZE_SEXPR;
            else fmt = PT_SERIALIZE_JSON;
        }
    }

    if (!file_path || !query_src) {
        fprintf(stderr, "Usage: prowsetk pdql -i <html_file> -q <query> [-s json|yaml|xml|sexpr]\n");
        return 1;
    }

    /* Read HTML file */
    FILE* f = fopen(file_path, "rb");
    if (!f) { perror("Failed to open input file"); return 1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* buf = (char*)malloc(sz + 1);
    fread(buf, 1, sz, f);
    buf[sz] = '\0';
    fclose(f);

    /* Construct Queryable DOM */
    pt_dom_document_t* doc = pt_dom_create_from_html(buf, sz);
    free(buf);

    /* Compile & Run PDQL */
    char* err = NULL;
    pt_pdql_query_t* q = pt_pdql_compile(query_src, &err);
    if (!q) {
        fprintf(stderr, "PDQL Compilation Error: %s\n", err ? err : "unknown");
        pt_dom_free(doc);
        return 1;
    }

    pt_pdql_result_t* res = pt_pdql_execute(q, doc);
    char* output = pt_pdql_serialize(res, fmt);
    printf("%s\n", output);

    /* Cleanup */
    pt_pdql_str_free(output);
    pt_pdql_result_free(res);
    pt_pdql_query_free(q);
    pt_dom_free(doc);

    return 0;
}
```

---
