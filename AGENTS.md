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
  `endpoint-extraction`, `storage`).
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
