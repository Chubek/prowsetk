cmake_minimum_required(VERSION 3.25)

project(ProwseTk
    VERSION 0.1.0
    DESCRIPTION "Embeddable, headless, programmable web browser toolkit"
    LANGUAGES C CXX)

option(PROWSETK_ENABLE_JAVASCRIPT
    "Build the QuickJS page JavaScript runtime" ON)
option(PROWSETK_BUILD_AI_ORACLE
    "Build the optional OpenAI oracle plugin when its headers are available" ON)
option(PROWSETK_BUILD_QUTE_REPL
    "Build the Qutebrowser Lua console when Replxx and Lua are available" ON)
option(PROWSETK_BUILD_PROWSE_GUI "Build the event-stream GUI consumer" ON)
option(PROWSETK_GFX_X11 "Build the optional X11 GFX adapter" OFF)
option(PROWSETK_GFX_FLTK "Build the optional FLTK GFX adapter" OFF)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(ProwseTkHelpers)
include(CompilerWarnings)
include(Dependencies)

option(PROWSETK_BUILD_TESTS    "Build the test suite"                    ON)
option(PROWSETK_BUILD_EXAMPLES "Build the example programs"              ON)
option(PROWSETK_BUILD_CLI      "Build the prowsetk command-line interface" ON)
option(PROWSETK_BUILD_FLATWORM_MODULES "Build the shipped native Flatworm modules" ON)
option(PROWSETK_ENABLE_WASM    "Enable the WebAssembly plugin runtime"   OFF)
set(PROWSETK_WASM_RUNTIME "wasmtime" CACHE STRING "WASM runtime backend")
option(PROWSETK_ENABLE_WASI    "Enable unrestricted WASI capabilities"   OFF)
option(PROWSETK_ENABLE_EBPF   "Enable the optional libbpf/eBPF interface" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)
if(PROWSETK_BUILD_TESTS)
    enable_testing()
endif()

add_subdirectory(src)

if(PROWSETK_BUILD_FLATWORM_MODULES)
    add_subdirectory(flatworm-modules)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/CMakeLists.txt")
    add_subdirectory(tools)
endif()

prowsetk_export_package()

if(PROWSETK_BUILD_TESTS)
    add_subdirectory(tests)
endif()

if(PROWSETK_BUILD_EXAMPLES AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/examples/CMakeLists.txt")
    add_subdirectory(examples)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/interface/pyprowsetk/CMakeLists.txt")
    add_subdirectory(interface/pyprowsetk)
endif()
