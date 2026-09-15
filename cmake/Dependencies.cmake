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
