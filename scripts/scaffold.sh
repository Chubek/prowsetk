#!/usr/bin/env bash
#
# scaffold.sh — create or refresh the ProwseTk project skeleton.
#
# Target directory selection:
#   $PROWSETK_DIR            (preferred)
#   <script dir>/..          (fallback: one directory above this script)
#
# Existing files are never overwritten unless --force is given. The generated
# .gitignore and .gitmodules are the canonical copies; keep the working tree in
# sync with them.
#
# Usage:
#   scripts/scaffold.sh [--submodules] [--force] [--help]

set -euo pipefail

FORCE=0

usage() {
    cat <<'USAGE_EOF'
scaffold.sh — create or refresh the ProwseTk project skeleton.

Usage:
  scripts/scaffold.sh [--submodules] [--force] [--help]

Options:
  -f, --force        overwrite existing files
  -s, --submodules   grab submodules
  -h, --help         show this help and exit

Environment:
  PROWSETK_DIR  target directory; defaults to the parent of this script.
USAGE_EOF
}

SUBMODULES=OFF

for arg in "$@"; do
    case "$arg" in
        -f|--force) FORCE=1 ;;
        -h|--help)  usage; exit 0 ;;
	-s|--submodules) SUBMODULES=ON  ;;
        *)
            printf 'scaffold.sh: unknown option: %s\n' "$arg" >&2
            usage >&2
            exit 2
            ;;
    esac
done

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
FALLBACK_ROOT="$(cd -- "${SCRIPT_DIR}/.." >/dev/null 2>&1 && pwd)"

if [[ -n "${PROWSETK_DIR:-}" ]]; then
    ROOT="${PROWSETK_DIR}"
else
    ROOT="${FALLBACK_ROOT}"
fi

mkdir -p -- "${ROOT}"
ROOT="$(cd -- "${ROOT}" >/dev/null 2>&1 && pwd)"

WROTE=0
SKIPPED=0

write_file() {
    local path="$1"
    if [[ -e "${path}" && "${FORCE}" -ne 1 ]]; then
        printf '  skip   %s\n' "${path#"${ROOT}"/}"
        SKIPPED=$((SKIPPED + 1))
        cat >/dev/null
        return 0
    fi
    mkdir -p -- "$(dirname -- "${path}")"
    cat >"${path}"
    printf '  write  %s\n' "${path#"${ROOT}"/}"
    WROTE=$((WROTE + 1))
}

make_dir() {
    mkdir -p -- "$1"
    printf '  dir    %s\n' "${1#"${ROOT}"/}"
}

printf 'Scaffolding ProwseTk into: %s\n' "${ROOT}"

# ---------------------------------------------------------------------------
# Directory tree
# ---------------------------------------------------------------------------
for d in \
    cmake \
    include/prowsetk \
    src \
    src/flatworm \
    src/plugins \
    tests \
    tests/unit \
    tests/integration \
    third_party \
    wit \
    lua \
    lua/lprowse \
    lua/lprowsext \
    plugins \
    drivers \
    examples \
    resources \
    scripts
do
    make_dir "${ROOT}/${d}"
done

# --------------------------------------------------------------------------
# Submodules
# --------------------------------------------------------------------------

if [[ $SUBMODULES = ON ]]; then
	sh $SCRIPTS_DIR/submodules.sh
fi

# ---------------------------------------------------------------------------
# .gitignore  (canonical copy)
# ---------------------------------------------------------------------------
write_file "${ROOT}/.gitignore" <<'GITIGNORE_EOF'
# ---------------------------------------------------------------------------
# Vendored third-party libraries
#
# Dependencies are declared in .gitmodules and materialized under third_party/
# with `git submodule update --init --recursive`. Submodule gitlinks already
# present in the index are unaffected by this rule; it only prevents stray
# vendored drops from being committed.
# ---------------------------------------------------------------------------
/third_party/

# ---------------------------------------------------------------------------
# Build output
# ---------------------------------------------------------------------------
/build/
/out/
/cmake-build-*/
CMakeCache.txt
CMakeFiles/
CMakeUserPresets.json
cmake_install.cmake
CTestTestfile.cmake
install_manifest.txt
compile_commands.json
Makefile
*.o
*.obj
*.lo
*.a
*.la
*.so
*.so.*
*.dylib
*.dll
*.lib

# ---------------------------------------------------------------------------
# ProwseTk runtime state
# ---------------------------------------------------------------------------
.prowse/
*.prowse-cache

# ---------------------------------------------------------------------------
# Test and coverage output
# ---------------------------------------------------------------------------
/Testing/
*.gcda
*.gcno
*.gcov
*.profraw
*.profdata
/test-results/
coverage/

# ---------------------------------------------------------------------------
# Tooling and environments
# ---------------------------------------------------------------------------
/env/
/venv/
/.venv/
__pycache__/
*.py[cod]

# ---------------------------------------------------------------------------
# Editors and OS metadata
# ---------------------------------------------------------------------------
.idea/
.vscode/
*.swp
*.swo
*~
.DS_Store
Thumbs.db
GITIGNORE_EOF

# ---------------------------------------------------------------------------
# .gitmodules  (canonical copy)
# ---------------------------------------------------------------------------
write_file "${ROOT}/.gitmodules" <<'GITMODULES_EOF'
# ---------------------------------------------------------------------------
# Vendored third-party dependencies.
#
# Populate with:
#   git submodule update --init --recursive
#
# Paths live under third_party/, which .gitignore excludes from normal tracking.
# ---------------------------------------------------------------------------

[submodule "third_party/c-ares"]
	path = third_party/c-ares
	url = https://github.com/c-ares/c-ares.git

[submodule "third_party/fmt"]
	path = third_party/fmt
	url = https://github.com/fmtlib/fmt.git

[submodule "third_party/gumbo-parser"]
	path = third_party/gumbo-parser
	url = https://github.com/google/gumbo-parser.git

[submodule "third_party/jemalloc"]
	path = third_party/jemalloc
	url = https://github.com/jemalloc/jemalloc.git

[submodule "third_party/inja"]
	path = third_party/inja
	url = https://github.com/pantor/inja.git

[submodule "third_party/kaguya"]
	path = third_party/kaguya
	url = https://github.com/satoren/kaguya.git

[submodule "third_party/lexbor"]
	path = third_party/lexbor
	url = https://github.com/lexbor/lexbor.git

[submodule "third_party/libdom"]
	path = third_party/libdom
	url = https://github.com/netsurf-browser/libdom.git

[submodule "third_party/libev"]
	path = third_party/libev
	url = https://github.com/enki/libev.git

[submodule "third_party/libmagic"]
	path = third_party/libmagic
	url = https://github.com/file/file.git

[submodule "third_party/libmill"]
	path = third_party/libmill
	url = https://github.com/sustrik/libmill.git

[submodule "third_party/llhttp"]
	path = third_party/llhttp
	url = https://github.com/nodejs/llhttp.git

[submodule "third_party/lua"]
	path = third_party/lua
	url = https://github.com/lua/lua.git

[submodule "third_party/mbedtls"]
	path = third_party/mbedtls
	url = https://github.com/Mbed-TLS/mbedtls.git

[submodule "third_party/nexus"]
	path = third_party/nexus
	url = https://github.com/cbodley/nexus.git

[submodule "third_party/pugixml"]
	path = third_party/pugixml
	url = https://github.com/zeux/pugixml.git

[submodule "third_party/quickjs"]
	path = third_party/quickjs
	url = https://github.com/quickjs-ng/quickjs.git

[submodule "third_party/re2"]
	path = third_party/re2
	url = https://github.com/google/re2.git

[submodule "third_party/simdjson"]
	path = third_party/simdjson
	url = https://github.com/simdjson/simdjson.git

[submodule "third_party/spdlog"]
	path = third_party/spdlog
	url = https://github.com/gabime/spdlog.git

[submodule "third_party/tomlplusplus"]
	path = third_party/tomlplusplus
	url = https://github.com/marzer/tomlplusplus.git

[submodule "third_party/uriparser"]
	path = third_party/uriparser
	url = https://github.com/uriparser/uriparser.git

[submodule "third_party/uvwasi"]
	path = third_party/uvwasi
	url = https://github.com/nodejs/uvwasi.git

[submodule "third_party/wasi-libc"]
	path = third_party/wasi-libc
	url = https://github.com/WebAssembly/wasi-libc.git

[submodule "third_party/wasi-sdk"]
	path = third_party/wasi-sdk
	url = https://github.com/WebAssembly/wasi-sdk.git

[submodule "third_party/wasmtime-cpp"]
	path = third_party/wasmtime-cpp
	url = https://github.com/bytecodealliance/wasmtime-cpp.git

[submodule "third_party/yaml-cpp"]
	path = third_party/yaml-cpp
	url = https://github.com/jbeder/yaml-cpp.git

[submodule "third_party/zstd"]
	path = third_party/zstd
	url = https://github.com/facebook/zstd.git
GITMODULES_EOF

# ---------------------------------------------------------------------------
# Top-level CMakeLists.txt
# ---------------------------------------------------------------------------
write_file "${ROOT}/CMakeLists.txt" <<'CMAKE_EOF'
cmake_minimum_required(VERSION 3.25)

project(ProwseTk
    VERSION 0.1.0
    DESCRIPTION "Embeddable, headless, programmable web browser toolkit"
    LANGUAGES C CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(ProwseTkHelpers)
include(CompilerWarnings)
include(Dependencies)

option(PROWSETK_BUILD_TESTS    "Build the test suite"                    ON)
option(PROWSETK_BUILD_EXAMPLES "Build the example programs"              ON)
option(PROWSETK_ENABLE_WASM    "Enable the WebAssembly plugin runtime"   OFF)
set(PROWSETK_WASM_RUNTIME "wasmtime" CACHE STRING "WASM runtime backend")
option(PROWSETK_ENABLE_WASI    "Enable unrestricted WASI capabilities"   OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)
if(PROWSETK_BUILD_TESTS)
    enable_testing()
endif()

add_subdirectory(src)

if(PROWSETK_BUILD_TESTS)
    add_subdirectory(tests)
endif()

if(PROWSETK_BUILD_EXAMPLES AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/examples/CMakeLists.txt")
    add_subdirectory(examples)
endif()
CMAKE_EOF

# ---------------------------------------------------------------------------
# CMakePresets.json
# ---------------------------------------------------------------------------
write_file "${ROOT}/CMakePresets.json" <<'PRESETS_EOF'
{
  "version": 6,
  "cmakeMinimumRequired": { "major": 3, "minor": 25, "patch": 0 },
  "configurePresets": [
    {
      "name": "default",
      "displayName": "Default (RelWithDebInfo, tests + examples)",
      "binaryDir": "${sourceDir}/build/${presetName}",
      "cacheVariables": {
        "CMAKE_BUILD_TYPE": "RelWithDebInfo",
        "CMAKE_EXPORT_COMPILE_COMMANDS": "ON",
        "PROWSETK_BUILD_TESTS": "ON",
        "PROWSETK_BUILD_EXAMPLES": "ON"
      }
    },
    {
      "name": "debug",
      "displayName": "Debug (tests + examples)",
      "binaryDir": "${sourceDir}/build/${presetName}",
      "cacheVariables": {
        "CMAKE_BUILD_TYPE": "Debug",
        "CMAKE_EXPORT_COMPILE_COMMANDS": "ON",
        "PROWSETK_BUILD_TESTS": "ON",
        "PROWSETK_BUILD_EXAMPLES": "ON"
      }
    },
    {
      "name": "release",
      "displayName": "Release (no tests, no examples)",
      "binaryDir": "${sourceDir}/build/${presetName}",
      "cacheVariables": {
        "CMAKE_BUILD_TYPE": "Release",
        "CMAKE_EXPORT_COMPILE_COMMANDS": "ON",
        "PROWSETK_BUILD_TESTS": "OFF",
        "PROWSETK_BUILD_EXAMPLES": "OFF"
      }
    },
    {
      "name": "asan",
      "displayName": "Debug + AddressSanitizer/UBSan (tests)",
      "inherits": "debug",
      "cacheVariables": {
        "CMAKE_C_FLAGS": "-fsanitize=address,undefined -fno-omit-frame-pointer",
        "CMAKE_CXX_FLAGS": "-fsanitize=address,undefined -fno-omit-frame-pointer",
        "CMAKE_EXE_LINKER_FLAGS": "-fsanitize=address,undefined",
        "CMAKE_SHARED_LINKER_FLAGS": "-fsanitize=address,undefined",
        "PROWSETK_BUILD_EXAMPLES": "OFF"
      }
    },
    {
      "name": "coverage",
      "displayName": "Coverage instrumentation (tests)",
      "inherits": "debug",
      "cacheVariables": {
        "CMAKE_C_FLAGS": "--coverage -O0 -g",
        "CMAKE_CXX_FLAGS": "--coverage -O0 -g",
        "CMAKE_EXE_LINKER_FLAGS": "--coverage",
        "CMAKE_SHARED_LINKER_FLAGS": "--coverage",
        "PROWSETK_BUILD_EXAMPLES": "OFF"
      }
    },
    {
      "name": "wasm",
      "displayName": "Default + WebAssembly runtime (tests)",
      "inherits": "default",
      "cacheVariables": {
        "PROWSETK_ENABLE_WASM": "ON",
        "PROWSETK_WASM_RUNTIME": "wasmtime",
        "PROWSETK_ENABLE_WASI": "OFF"
      }
    }
  ],
  "buildPresets": [
    { "name": "default",  "configurePreset": "default" },
    { "name": "debug",    "configurePreset": "debug" },
    { "name": "release",  "configurePreset": "release" },
    { "name": "asan",     "configurePreset": "asan" },
    { "name": "coverage", "configurePreset": "coverage" },
    { "name": "wasm",     "configurePreset": "wasm" }
  ],
  "testPresets": [
    {
      "name": "default",
      "configurePreset": "default",
      "output": { "outputOnFailure": true },
      "execution": { "noTestsAction": "error" }
    },
    {
      "name": "debug",
      "configurePreset": "debug",
      "output": { "outputOnFailure": true },
      "execution": { "noTestsAction": "error" }
    },
    {
      "name": "asan",
      "configurePreset": "asan",
      "output": { "outputOnFailure": true },
      "execution": { "noTestsAction": "error" }
    },
    {
      "name": "coverage",
      "configurePreset": "coverage",
      "output": { "outputOnFailure": true },
      "execution": { "noTestsAction": "error" }
    },
    {
      "name": "wasm",
      "configurePreset": "wasm",
      "output": { "outputOnFailure": true },
      "execution": { "noTestsAction": "error" }
    }
  ],
  "workflowPresets": [
    {
      "name": "ci",
      "displayName": "CI: default",
      "steps": [
        { "type": "configure", "name": "default" },
        { "type": "build",     "name": "default" },
        { "type": "test",      "name": "default" }
      ]
    },
    {
      "name": "ci-asan",
      "displayName": "CI: AddressSanitizer/UBSan",
      "steps": [
        { "type": "configure", "name": "asan" },
        { "type": "build",     "name": "asan" },
        { "type": "test",      "name": "asan" }
      ]
    },
    {
      "name": "ci-wasm",
      "displayName": "CI: WebAssembly runtime",
      "steps": [
        { "type": "configure", "name": "wasm" },
        { "type": "build",     "name": "wasm" },
        { "type": "test",      "name": "wasm" }
      ]
    }
  ]
}
PRESETS_EOF

# ---------------------------------------------------------------------------
# cmake/ helper modules
# ---------------------------------------------------------------------------
write_file "${ROOT}/cmake/ProwseTkHelpers.cmake" <<'HELPERS_EOF'
include_guard(GLOBAL)

# prowsetk_add_library(<name> <sources...>)
# Creates a static library, exposes <PROJECT_SOURCE_DIR>/include publicly, and
# applies the shared warning set.
function(prowsetk_add_library name)
    add_library(${name} STATIC ${ARGN})
    target_include_directories(${name} PUBLIC
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:include>")
    target_compile_features(${name} PUBLIC cxx_std_20)
    prowsetk_set_warnings(${name})
endfunction()

# prowsetk_add_executable(<name> <sources...>)
function(prowsetk_add_executable name)
    add_executable(${name} ${ARGN})
    target_include_directories(${name} PRIVATE "${PROJECT_SOURCE_DIR}/include")
    prowsetk_set_warnings(${name})
endfunction()
HELPERS_EOF

write_file "${ROOT}/cmake/CompilerWarnings.cmake" <<'WARNINGS_EOF'
include_guard(GLOBAL)

# prowsetk_set_warnings(<target>)
# Single place that decides the project warning set. Do not duplicate flags in
# individual target definitions.
function(prowsetk_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${target} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wshadow)
    endif()
endfunction()
WARNINGS_EOF

write_file "${ROOT}/cmake/Dependencies.cmake" <<'DEPS_EOF'
include_guard(GLOBAL)

# Declarative dependency wiring. This is the single place that names third-party
# packages. Locate each dependency with find_package(... CONFIG) and, when it is
# not found, fall back to vendored sources under third_party/. Every dependency
# must be exposed as a ProwseTk::<name> imported target.
#
# Example:
#   find_package(fmt CONFIG QUIET)
#   if(NOT fmt_FOUND)
#       add_subdirectory("${PROJECT_SOURCE_DIR}/third_party/fmt" EXCLUDE_FROM_ALL)
#   endif()
#   add_library(ProwseTk::fmt ALIAS fmt::fmt)
DEPS_EOF

# ---------------------------------------------------------------------------
# Public headers
# ---------------------------------------------------------------------------
write_file "${ROOT}/include/prowsetk/version.hpp" <<'VERSION_EOF'
#ifndef PROWSETK_VERSION_HPP
#define PROWSETK_VERSION_HPP

#define PROWSETK_VERSION_MAJOR 0
#define PROWSETK_VERSION_MINOR 1
#define PROWSETK_VERSION_PATCH 0
#define PROWSETK_VERSION_STRING "0.1.0"

namespace prowsetk {

const char* version() noexcept;

}  // namespace prowsetk

#endif  // PROWSETK_VERSION_HPP
VERSION_EOF

write_file "${ROOT}/include/prowsetk/ProwseTk-Plugin.h" <<'PLUGIN_EOF'
#ifndef PROWSETK_PLUGIN_H
#define PROWSETK_PLUGIN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PROWSETK_PLUGIN_ABI_VERSION 1

typedef struct ProwseTkHost ProwseTkHost;

typedef enum {
    PROWSETK_PLUGIN_NATIVE = 1,
    PROWSETK_PLUGIN_LUA = 2,
    PROWSETK_PLUGIN_WASM = 3
} ProwseTkPluginType;

typedef struct {
    const char* name;
    const char* version;
    const char* abi_version;
    const char* description;
    ProwseTkPluginType type;
    const char* const* capabilities;
    size_t capability_count;
} ProwseTkPluginInfo;

typedef struct {
    int (*initialize)(ProwseTkHost* host);
    void (*shutdown)(ProwseTkHost* host);
    const ProwseTkPluginInfo* (*info)(void);
} ProwseTkPlugin;

#ifdef __cplusplus
}
#endif

#endif /* PROWSETK_PLUGIN_H */
PLUGIN_EOF

# ---------------------------------------------------------------------------
# Core library
# ---------------------------------------------------------------------------
write_file "${ROOT}/src/CMakeLists.txt" <<'SRC_CMAKE_EOF'
prowsetk_add_library(prowsetk_core
    prowsetk.cpp)

add_library(ProwseTk::core ALIAS prowsetk_core)
SRC_CMAKE_EOF

write_file "${ROOT}/src/prowsetk.cpp" <<'SRC_CPP_EOF'
#include <prowsetk/version.hpp>

namespace prowsetk {

const char* version() noexcept {
    return PROWSETK_VERSION_STRING;
}

}  // namespace prowsetk
SRC_CPP_EOF

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
write_file "${ROOT}/tests/CMakeLists.txt" <<'TESTS_CMAKE_EOF'
include(CTest)
enable_testing()

add_subdirectory(unit)
add_subdirectory(integration)
TESTS_CMAKE_EOF

write_file "${ROOT}/tests/unit/CMakeLists.txt" <<'UNIT_CMAKE_EOF'
prowsetk_add_executable(prowsetk_unit_smoke
    test_smoke.cpp)
target_link_libraries(prowsetk_unit_smoke PRIVATE ProwseTk::core)

add_test(NAME prowsetk.unit.smoke COMMAND prowsetk_unit_smoke)
set_tests_properties(prowsetk.unit.smoke PROPERTIES
    TIMEOUT 30
    LABELS "unit;core")
UNIT_CMAKE_EOF

write_file "${ROOT}/tests/unit/test_smoke.cpp" <<'UNIT_SMOKE_EOF'
#include <prowsetk/version.hpp>

#include <cstdlib>
#include <cstring>
#include <iostream>

int main() {
    const char* v = prowsetk::version();
    if (v == nullptr || std::strlen(v) == 0) {
        std::cerr << "prowsetk.unit.smoke: empty version string\n";
        return EXIT_FAILURE;
    }
    std::cout << "prowsetk " << v << '\n';
    return EXIT_SUCCESS;
}
UNIT_SMOKE_EOF

write_file "${ROOT}/tests/integration/CMakeLists.txt" <<'INTEG_CMAKE_EOF'
prowsetk_add_executable(prowsetk_integration_smoke
    test_smoke_integration.cpp)
target_link_libraries(prowsetk_integration_smoke PRIVATE ProwseTk::core)

add_test(NAME prowsetk.integration.smoke COMMAND prowsetk_integration_smoke)
set_tests_properties(prowsetk.integration.smoke PROPERTIES
    TIMEOUT 60
    LABELS "integration;core")
INTEG_CMAKE_EOF

write_file "${ROOT}/tests/integration/test_smoke_integration.cpp" <<'INTEG_SMOKE_EOF'
#include <prowsetk/version.hpp>

#include <cstdlib>
#include <iostream>

int main() {
    std::cout << "prowsetk.integration.smoke: core " << prowsetk::version() << '\n';
    return EXIT_SUCCESS;
}
INTEG_SMOKE_EOF

# ---------------------------------------------------------------------------
# WIT interfaces
# ---------------------------------------------------------------------------
write_file "${ROOT}/wit/prowsetk-plugin.wit" <<'WIT_EOF'
package prowsetk:plugin@0.1.0;

interface types {
    record request {
        method: string,
        url: string,
        headers: list<tuple<string, string>>,
        body: list<u8>,
    }

    record response {
        status: u16,
        headers: list<tuple<string, string>>,
        body: list<u8>,
    }

    variant hook-result {
        continue,
        reject(string),
        replace-request(request),
    }
}

interface network-hooks {
    before-request: func(req: types.request) -> result<types.hook-result, string>;
    after-response: func(req: types.request, res: types.response)
        -> result<types.hook-result, string>;
}

interface document-extractor {
    extract: func(url: string, html: string) -> result<string, string>;
}

world prowsetk-plugin {
    export network-hooks;
    export document-extractor;
}
WIT_EOF

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
printf '\n'
printf 'Scaffold complete: %d file(s) written, %d skipped.\n' "${WROTE}" "${SKIPPED}"

if [[ ! -f "${ROOT}/README.md" ]]; then
    printf 'WARNING: README.md is missing; it is the architecture of record.\n' >&2
fi
if [[ ! -f "${ROOT}/AGENTS.md" ]]; then
    printf 'WARNING: AGENTS.md is missing; implementing agents rely on it.\n' >&2
fi

printf '\nNext steps:\n'
printf '  1. git submodule update --init --recursive\n'
printf '  2. cmake --preset default\n'
printf '  3. cmake --build --preset default\n'
printf '  4. ctest --preset default\n'
