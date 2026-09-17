include_guard(GLOBAL)

find_package(OpenSSL REQUIRED COMPONENTS Crypto)
add_library(ProwseTk::crypto ALIAS OpenSSL::Crypto)

# Tokyo Cabinet is used by the optional persistent session backend.
set(PROWSETK_TCB_SOURCE_DIR "${PROJECT_SOURCE_DIR}/third_party/Tokyo-Cabinet")
if(EXISTS "${PROWSETK_TCB_SOURCE_DIR}/tcbdb.c")
    add_library(prowsetk_tokyocabinet STATIC
        "${PROWSETK_TCB_SOURCE_DIR}/tcutil.c"
        "${PROWSETK_TCB_SOURCE_DIR}/tchdb.c"
        "${PROWSETK_TCB_SOURCE_DIR}/tcbdb.c"
        "${PROWSETK_TCB_SOURCE_DIR}/tcfdb.c"
        "${PROWSETK_TCB_SOURCE_DIR}/tctdb.c"
        "${PROWSETK_TCB_SOURCE_DIR}/tcadb.c"
        "${PROWSETK_TCB_SOURCE_DIR}/myconf.c"
        "${PROWSETK_TCB_SOURCE_DIR}/md5.c")
    set_target_properties(prowsetk_tokyocabinet PROPERTIES
        EXPORT_NAME tokyocabinet POSITION_INDEPENDENT_CODE ON)
    target_include_directories(prowsetk_tokyocabinet PRIVATE
        "${PROWSETK_TCB_SOURCE_DIR}")
    target_link_libraries(prowsetk_tokyocabinet PUBLIC m pthread z bz2)
    add_library(ProwseTk::tokyocabinet ALIAS prowsetk_tokyocabinet)
    prowsetk_install_library(prowsetk_tokyocabinet)
endif()

# Declarative dependency wiring. This is the single place that names third-party
# packages. Locate each dependency with find_package(... CONFIG) and, when it is
# not found, fall back to vendored sources under third_party/. Every dependency
# must be exposed as a ProwseTk::<name> imported target.

# ---------------------------------------------------------------------------
# Lua — required for the lprowse automation layer, optional for the core.
# ---------------------------------------------------------------------------
find_package(Lua QUIET)
if(Lua_FOUND)
    set(PROWSETK_HAVE_LUA ON CACHE INTERNAL "Lua runtime available")
else()
    set(PROWSETK_HAVE_LUA OFF CACHE INTERNAL "Lua runtime available")
    message(STATUS "ProwseTk: Lua not found; lprowse bindings disabled")
endif()

# ---------------------------------------------------------------------------
# QuickJS — optional page JavaScript runtime. When the vendored source is
# absent the build falls back to the null JavaScript runtime.
# ---------------------------------------------------------------------------
if(PROWSETK_ENABLE_JAVASCRIPT)
    set(PROWSETK_QUICKJS_SOURCE_DIR
        "${PROJECT_SOURCE_DIR}/third_party/quickjs")
    if(EXISTS "${PROWSETK_QUICKJS_SOURCE_DIR}/CMakeLists.txt")
        set(QJS_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
        set(QJS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(QJS_BUILD_CLI_STATIC OFF CACHE BOOL "" FORCE)
        set(QJS_BUILD_CLI_WITH_MIMALLOC OFF CACHE BOOL "" FORCE)
        set(QJS_BUILD_CLI_WITH_STATIC_MIMALLOC OFF CACHE BOOL "" FORCE)
        add_subdirectory("${PROWSETK_QUICKJS_SOURCE_DIR}"
                         "${CMAKE_BINARY_DIR}/third_party/quickjs"
                         EXCLUDE_FROM_ALL)
        set_target_properties(qjs PROPERTIES POSITION_INDEPENDENT_CODE ON)
        set(PROWSETK_HAVE_QUICKJS ON CACHE INTERNAL
            "QuickJS runtime available")
        add_library(ProwseTk::quickjs ALIAS qjs)
        message(STATUS "ProwseTk: using vendored QuickJS runtime")
    else()
        set(PROWSETK_HAVE_QUICKJS OFF CACHE INTERNAL
            "QuickJS runtime available")
        message(STATUS
                "ProwseTk: QuickJS source missing; JavaScript runtime disabled")
    endif()
else()
    set(PROWSETK_HAVE_QUICKJS OFF CACHE INTERNAL
        "QuickJS runtime available")
endif()

# ---------------------------------------------------------------------------
# pugixml — optional XPath 1.0 engine for flatworm DOM queries. Vendored under
# third_party/pugixml and exposed as ProwseTk::pugixml.
# ---------------------------------------------------------------------------
set(PROWSETK_PUGIXML_SOURCE_DIR "${PROJECT_SOURCE_DIR}/third_party/pugixml")
if(EXISTS "${PROWSETK_PUGIXML_SOURCE_DIR}/src/pugixml.cpp")
    if(NOT TARGET prowsetk_pugixml)
        add_library(prowsetk_pugixml STATIC
            "${PROWSETK_PUGIXML_SOURCE_DIR}/src/pugixml.cpp")
        set_target_properties(prowsetk_pugixml PROPERTIES
            POSITION_INDEPENDENT_CODE ON)
        target_include_directories(prowsetk_pugixml PUBLIC
            "$<BUILD_INTERFACE:${PROWSETK_PUGIXML_SOURCE_DIR}/src>"
            "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
        install(FILES
            "${PROWSETK_PUGIXML_SOURCE_DIR}/src/pugiconfig.hpp"
            "${PROWSETK_PUGIXML_SOURCE_DIR}/src/pugixml.hpp"
            DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
        add_library(ProwseTk::pugixml ALIAS prowsetk_pugixml)
    endif()
    set(PROWSETK_HAVE_PUGIXML ON CACHE INTERNAL "pugixml available")
    message(STATUS "ProwseTk: using vendored pugixml for XPath")
else()
    set(PROWSETK_HAVE_PUGIXML OFF CACHE INTERNAL "pugixml available")
    message(STATUS "ProwseTk: pugixml missing; XPath disabled")
endif()

# ---------------------------------------------------------------------------
# tomlplusplus — optional Prowse.toml project-configuration parsing. Vendored
# single-header library exposed as ProwseTk::tomlplusplus.
# ---------------------------------------------------------------------------
set(PROWSETK_TOMLPLUSPLUS_SOURCE_DIR
    "${PROJECT_SOURCE_DIR}/third_party/tomlplusplus")
if(EXISTS "${PROWSETK_TOMLPLUSPLUS_SOURCE_DIR}/toml.hpp")
    if(NOT TARGET prowsetk_tomlplusplus)
        add_library(prowsetk_tomlplusplus INTERFACE)
        target_include_directories(prowsetk_tomlplusplus INTERFACE
            "$<BUILD_INTERFACE:${PROWSETK_TOMLPLUSPLUS_SOURCE_DIR}>"
            "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
        install(FILES "${PROWSETK_TOMLPLUSPLUS_SOURCE_DIR}/toml.hpp"
            DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
        add_library(ProwseTk::tomlplusplus ALIAS prowsetk_tomlplusplus)
    endif()
    set(PROWSETK_HAVE_TOMLPLUSPLUS ON CACHE INTERNAL
        "tomlplusplus available")
    message(STATUS "ProwseTk: using vendored tomlplusplus for Prowse.toml")
else()
    set(PROWSETK_HAVE_TOMLPLUSPLUS OFF CACHE INTERNAL
        "tomlplusplus available")
    message(STATUS "ProwseTk: tomlplusplus missing; Prowse.toml disabled")
endif()

# ---------------------------------------------------------------------------
# GoogleTest — used by the test suite when available.
# ---------------------------------------------------------------------------
if(PROWSETK_BUILD_TESTS)
    find_package(GTest QUIET)
    if(GTest_FOUND)
        set(PROWSETK_HAVE_GTEST ON CACHE INTERNAL "GoogleTest available")
    else()
        set(PROWSETK_HAVE_GTEST OFF CACHE INTERNAL "GoogleTest available")
        message(STATUS "ProwseTk: GoogleTest not found; using fallback runner")
    endif()
endif()
