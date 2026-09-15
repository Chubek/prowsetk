include_guard(GLOBAL)

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
