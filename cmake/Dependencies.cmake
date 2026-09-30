include_guard(GLOBAL)

# OpenSSL's supported CMake discovery uses FindOpenSSL. HTTPS remains optional;
# the vendored upstream uses its own Configure build, so install it first when
# a system package is unavailable (OPENSSL_ROOT_DIR selects that installation).
find_package(OpenSSL 3.0 QUIET COMPONENTS SSL Crypto)
set(PROWSETK_HAVE_OPENSSL ${OpenSSL_FOUND} CACHE INTERNAL "Verified HTTPS available")
if(OpenSSL_FOUND)
    add_library(ProwseTk::tls ALIAS OpenSSL::SSL)
    message(STATUS "ProwseTk: OpenSSL enables verified HTTPS")
else()
    message(STATUS "ProwseTk: OpenSSL unavailable; socket transport is HTTP only")
endif()

set(PROWSETK_LIBTOMCRYPT_SOURCE_DIR
    "${PROJECT_SOURCE_DIR}/third_party/libtomcrypt")
if(EXISTS "${PROWSETK_LIBTOMCRYPT_SOURCE_DIR}/CMakeLists.txt")
    set(WITH_LTM OFF CACHE BOOL "" FORCE)
    set(WITH_TFM OFF CACHE BOOL "" FORCE)
    set(WITH_GMP OFF CACHE BOOL "" FORCE)
    set(WITH_PTHREAD OFF CACHE BOOL "" FORCE)
    set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(BUILD_USEFUL_DEMOS OFF CACHE BOOL "" FORCE)
    set(BUILD_USABLE_DEMOS OFF CACHE BOOL "" FORCE)
    set(BUILD_TEST_DEMOS OFF CACHE BOOL "" FORCE)
    set(INSTALL_DEMOS OFF CACHE BOOL "" FORCE)
    add_subdirectory("${PROWSETK_LIBTOMCRYPT_SOURCE_DIR}"
                     "${CMAKE_BINARY_DIR}/third_party/libtomcrypt"
                     EXCLUDE_FROM_ALL)
    set_target_properties(libtomcrypt PROPERTIES
        EXPORT_NAME tomcrypt POSITION_INDEPENDENT_CODE ON)
    target_compile_options(libtomcrypt PRIVATE
        $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-w>)
    add_library(ProwseTk::tomcrypt ALIAS libtomcrypt)
    # libtomcrypt declares PUBLIC_HEADER as paths relative to its own source
    # dir (see third_party/libtomcrypt/sources.cmake). An install() call in
    # this directory would resolve those against PROJECT_SOURCE_DIR, yielding
    # "<source>/src/headers/..." which does not exist. Re-root them to
    # absolute paths before exporting/installing. Do not patch third_party/.
    get_target_property(_prowsetk_ltc_public_header libtomcrypt PUBLIC_HEADER)
    if(_prowsetk_ltc_public_header)
        set(_prowsetk_ltc_abs_headers "")
        foreach(_prowsetk_ltc_h IN LISTS _prowsetk_ltc_public_header)
            if(IS_ABSOLUTE "${_prowsetk_ltc_h}")
                list(APPEND _prowsetk_ltc_abs_headers "${_prowsetk_ltc_h}")
            else()
                list(APPEND _prowsetk_ltc_abs_headers
                    "${PROWSETK_LIBTOMCRYPT_SOURCE_DIR}/${_prowsetk_ltc_h}")
            endif()
        endforeach()
        set_target_properties(libtomcrypt PROPERTIES
            PUBLIC_HEADER "${_prowsetk_ltc_abs_headers}")
        unset(_prowsetk_ltc_abs_headers)
        unset(_prowsetk_ltc_h)
    endif()
    unset(_prowsetk_ltc_public_header)
    # Upstream exposes only <libtomcrypt/tomcrypt.h> for installs, but
    # ProwseTk sources include <tomcrypt.h>; expose the bare include dir too
    # so the installed ProwseTk::tomcrypt target keeps working.
    target_include_directories(libtomcrypt PUBLIC
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
    prowsetk_install_library(libtomcrypt)
    message(STATUS "ProwseTk: using vendored LibTomCrypt")
else()
    message(FATAL_ERROR
            "ProwseTk: third_party/libtomcrypt is required for encrypted storage")
endif()

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
# libHaru — PDF generation for the standalone page2pdf IR compiler.
# ---------------------------------------------------------------------------
find_package(libharu CONFIG QUIET)
if(TARGET libharu::hpdf)
    add_library(ProwseTk::haru ALIAS libharu::hpdf)
    set(PROWSETK_HAVE_HARU ON CACHE INTERNAL "libHaru PDF generator available")
    message(STATUS "ProwseTk: using system libHaru for page2pdf")
elseif(TARGET hpdf)
    add_library(ProwseTk::haru ALIAS hpdf)
    set(PROWSETK_HAVE_HARU ON CACHE INTERNAL "libHaru PDF generator available")
elseif(EXISTS "${PROJECT_SOURCE_DIR}/third_party/libharu/CMakeLists.txt")
    set(PROWSETK_LIBHARU_SOURCE_DIR "${PROJECT_SOURCE_DIR}/third_party/libharu")
    set(PROWSETK_LIBHARU_BINARY_DIR "${CMAKE_BINARY_DIR}/third_party/libharu")
    set(LIBHPDF_EXAMPLES OFF CACHE BOOL "Build libHaru examples" FORCE)
    add_subdirectory("${PROWSETK_LIBHARU_SOURCE_DIR}"
                     "${PROWSETK_LIBHARU_BINARY_DIR}" EXCLUDE_FROM_ALL)
    # libHaru's upstream directory-scoped include paths do not propagate to a
    # sibling consumer. Publish both its source headers and generated config.
    target_include_directories(hpdf PUBLIC
        "$<BUILD_INTERFACE:${PROWSETK_LIBHARU_SOURCE_DIR}/include>"
        "$<BUILD_INTERFACE:${PROWSETK_LIBHARU_BINARY_DIR}/include>")
    set_target_properties(hpdf PROPERTIES POSITION_INDEPENDENT_CODE ON)
    add_library(ProwseTk::haru ALIAS hpdf)
    set(PROWSETK_HAVE_HARU ON CACHE INTERNAL "libHaru PDF generator available")
    message(STATUS "ProwseTk: using vendored libHaru for page2pdf")
else()
    set(PROWSETK_HAVE_HARU OFF CACHE INTERNAL "libHaru PDF generator available")
    message(STATUS "ProwseTk: libHaru missing; page2pdf disabled")
endif()

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

# Optional HTML5 document adapters and terminal frontend.
option(PROWSETK_ENABLE_HTML5 "Build vendored HTML5 document parsers" ON)
if(PROWSETK_ENABLE_HTML5 AND EXISTS "${PROJECT_SOURCE_DIR}/third_party/lexbor/CMakeLists.txt")
    set(LEXBOR_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(LEXBOR_BUILD_STATIC ON CACHE BOOL "" FORCE)
    set(LEXBOR_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    add_subdirectory("${PROJECT_SOURCE_DIR}/third_party/lexbor" "${CMAKE_BINARY_DIR}/third_party/lexbor" EXCLUDE_FROM_ALL)
    add_library(ProwseTk::lexbor ALIAS lexbor_static)
    prowsetk_install_library(lexbor_static)
    set(PROWSETK_HAVE_LEXBOR ON)
endif()
if(PROWSETK_ENABLE_HTML5 AND EXISTS "${PROJECT_SOURCE_DIR}/third_party/gumbo-parser/src/gumbo.h")
    set(_gumbo "${PROJECT_SOURCE_DIR}/third_party/gumbo-parser/src")
    add_library(prowsetk_gumbo STATIC
        ${_gumbo}/attribute.c ${_gumbo}/char_ref.c ${_gumbo}/error.c
        ${_gumbo}/parser.c ${_gumbo}/string_buffer.c ${_gumbo}/string_piece.c
        ${_gumbo}/tag.c ${_gumbo}/tokenizer.c ${_gumbo}/utf8.c
        ${_gumbo}/util.c ${_gumbo}/vector.c)
    set_target_properties(prowsetk_gumbo PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_include_directories(prowsetk_gumbo PUBLIC "$<BUILD_INTERFACE:${_gumbo}>")
    add_library(ProwseTk::gumbo ALIAS prowsetk_gumbo)
    prowsetk_install_library(prowsetk_gumbo)
    set(PROWSETK_HAVE_GUMBO ON)
endif()
if(EXISTS "${PROJECT_SOURCE_DIR}/third_party/termbox2/termbox2.h")
    add_library(prowsetk_termbox INTERFACE)
    target_include_directories(prowsetk_termbox INTERFACE "${PROJECT_SOURCE_DIR}/third_party/termbox2")
    add_library(ProwseTk::termbox ALIAS prowsetk_termbox)
endif()

# libmnl is an optional netlink helper for host networking diagnostics. It is
# intentionally not used for proxy transport; HTTP/SOCKS proxying stays in the
# host-mediated NetworkClient and works without libmnl.
if(EXISTS "${PROJECT_SOURCE_DIR}/third_party/libmnl/src/socket.c")
    add_library(prowsetk_libmnl STATIC
        "${PROJECT_SOURCE_DIR}/third_party/libmnl/src/socket.c"
        "${PROJECT_SOURCE_DIR}/third_party/libmnl/src/nlmsg.c"
        "${PROJECT_SOURCE_DIR}/third_party/libmnl/src/attr.c"
        "${PROJECT_SOURCE_DIR}/third_party/libmnl/src/callback.c")
    target_include_directories(prowsetk_libmnl PUBLIC
        "${PROJECT_SOURCE_DIR}/third_party/libmnl/include"
        "${PROJECT_SOURCE_DIR}/third_party/libmnl/src")
    add_library(ProwseTk::mnl ALIAS prowsetk_libmnl)
endif()
