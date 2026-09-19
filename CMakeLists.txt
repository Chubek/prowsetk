cmake_minimum_required(VERSION 3.25)

project(ProwseTk
    VERSION 0.1.0
    DESCRIPTION "Embeddable, headless, programmable web browser toolkit"
    LANGUAGES C CXX)

option(PROWSETK_ENABLE_JAVASCRIPT
    "Build the QuickJS page JavaScript runtime" ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(ProwseTkHelpers)
include(CompilerWarnings)
include(Dependencies)

option(PROWSETK_BUILD_TESTS    "Build the test suite"                    ON)
option(PROWSETK_BUILD_EXAMPLES "Build the example programs"              ON)
option(PROWSETK_BUILD_CLI      "Build the prowsetk command-line interface" ON)
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
