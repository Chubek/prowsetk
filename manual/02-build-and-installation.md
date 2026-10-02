# Chapter 02: Build and Installation

[Manual index](README.md)

## Prerequisites and checkout

Use CMake 3.25 or newer, a C/C++ toolchain supporting C++20, and Git. Populate
the vendored dependencies before configuring:

```sh
git submodule update --init --recursive
bash scripts/scaffold.sh
cmake --preset default
cmake --build --preset default -j 4
ctest --preset default
```

`scripts/scaffold.sh` refreshes the repository skeleton. It targets
`$PROWSETK_DIR` when set, otherwise the directory above the script. Build outputs
belong under `build/`; vendored source remains under `third_party/`.

The current root build includes the session-support library, which requires
`third_party/libtomcrypt` and `third_party/Tokyo-Cabinet`. Python bindings need
Python 3.9+ development headers and `third_party/nanobind` (including its
submodules). Ensure those source directories are populated; Tokyo Cabinet and
nanobind are referenced by the build but are not currently entries in
`.gitmodules`. Their repository URLs are listed in `scripts/submodules.sh`.
Use `-DPROWSETK_BUILD_PYTHON=OFF` when Python bindings are not needed.

## Presets

Every preset has matching configure, build, and test names.

| Preset | Purpose |
|---|---|
| `default` | RelWithDebInfo, tests and examples enabled |
| `debug` | Debug development build with tests |
| `release` | Optimized build, tests and examples disabled |
| `asan` | Debug with AddressSanitizer and UndefinedBehaviorSanitizer |
| `coverage` | Coverage-instrumented tests |
| `wasm` | WASM-enabled configuration; currently tests the disabled-runtime path |

For example, use `cmake --preset asan`, `cmake --build --preset asan`, and
`ctest --preset asan`. The CI workflows are `ci`, `ci-asan`, and `ci-wasm`:
`cmake --workflow --preset ci` runs configure/build/test in sequence.

## Important build options

| Option | Default | Meaning |
|---|---|---|
| `PROWSETK_BUILD_TESTS` | `ON` | Register and build CTest suites |
| `PROWSETK_BUILD_EXAMPLES` | `ON` | Build C++/Lua examples |
| `PROWSETK_BUILD_CLI` | `ON` | Build `prowsetk` |
| `PROWSETK_ENABLE_JAVASCRIPT` | `ON` | Use QuickJS when available |
| `PROWSETK_ENABLE_WASM` | `OFF` | Select optional WASM integration |
| `PROWSETK_WASM_RUNTIME` | `wasmtime` | Runtime selection behind `WasmRuntime` |
| `PROWSETK_ENABLE_WASI` | `OFF` | WASI capability setting |
| `PROWSETK_ENABLE_EBPF` | `OFF` | Enable optional libbpf integration |
| `PROWSETK_BUILD_SPIDER` | `ON` | Build spider when Linux/LMDB/lmdbxx are available |
| `PROWSETK_BUILD_PYTHON` | `ON` | Build Python bindings when Python development support is found |
| `PROWSETK_BUILD_TUI` | `ON` | Build terminal tools when the Termlib/Termscript source is available |

Dependency discovery is centralized in `cmake/Dependencies.cmake`. Lua enables
driver APIs; tomlplusplus enables project and tool configuration; pugixml enables
XPath; OpenSSL 3 enables certificate-verified HTTPS; libHaru enables `page2pdf`.
Python bindings also need their Python/nanobind toolchain. The root README lists
the complete dependency inventory.

`crawler` builds on POSIX with Lua and tomlplusplus. Pagewatch requires Linux
and those libraries. Spider additionally requires LMDB and lmdbxx. Missing
optional dependencies produce a disabled feature or omit its target; inspect
the configure output before assuming a binary was built.

Prowse-TUI requires the source tree at `third_party/termlib`, including
`termlib.h`, its Termscript grammar/runtime, and parser-generator script, plus
Perl. This source tree is not currently declared in `.gitmodules`; without it,
the TUI target is omitted. The required storage sources and nanobind when Python
is enabled must be present for the build to configure successfully.

## Running and installing

Use build-tree paths directly, or add the directories you need to `PATH`:

```sh
export PATH="$PWD/build/default/src/cli:$PWD/build/default/tools/crawler:$PWD/build/default/tools/pagewatch:$PATH"
prowsetk version
cmake --install build/default --prefix "$PWD/build/install"
export PATH="$PWD/build/install/bin:$PATH"
```

Installations include executables, public headers, the exported CMake package,
and plugin/tool data under `share/prowsetk/`. Crawler and pagewatch embed their
Lua modules. Other driver/plugin Lua recipes need the corresponding source or
installed module path described in [Chapter 11](11-lua-extensions.md).

## Tests and documentation

`ctest --preset default -R pdql --output-on-failure` selects a component.
CTest cases have labels and finite timeouts; the encrypted-storage tests have
a 300-second timeout for production-cost key derivation under instrumentation.
Python modules and stubs are isolated per preset. CTest configures sanitizer
preloading for Python.

Build this manual with `bash scripts/build-docs.sh`. Pandoc produces combined
HTML and LaTeX source under `build/docs`; a TeX engine is a separate requirement
for compiling LaTeX to PDF.

If configuration omits a target, check its dependency rather than looking for
a different executable name. Use `scripts/verify-libs-installed.sh` as documented
by that script when a library installation is in doubt. HTTPS failures should
be investigated through OpenSSL availability and trust configuration; an
HTTP-only build remains usable.

Reference: `CMakeLists.txt`, `CMakePresets.json`, `cmake/`, `scripts/build-docs.sh`.

**Next:** [Quick start and CLI](03-quick-start-and-cli.md).
