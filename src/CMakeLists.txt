set(PROWSETK_CORE_SOURCES
    prowsetk.cpp
    core/error.cpp
    core/capability.cpp
    core/event.cpp
    core/url.cpp
    core/redaction.cpp
    core/storage.cpp
    core/network_client.cpp
    core/javascript_runtime.cpp
    core/web_platform.cpp
    core/wasm_runtime.cpp
    core/plugin_registry.cpp
    core/browser.cpp
    core/lua_runtime.cpp
    flatworm/html_parser.cpp
    flatworm/css_selector.cpp
    flatworm/dom.cpp
    endpoint/endpoint_extraction.cpp)

prowsetk_add_library(prowsetk_core ${PROWSETK_CORE_SOURCES})

target_include_directories(prowsetk_core PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(prowsetk_core PUBLIC ${CMAKE_DL_LIBS})
if(PROWSETK_HAVE_LUA)
    target_include_directories(prowsetk_core PRIVATE ${LUA_INCLUDE_DIR})
    target_link_libraries(prowsetk_core PUBLIC ${LUA_LIBRARIES})
    target_compile_definitions(prowsetk_core PRIVATE PROWSETK_HAVE_LUA=1)
endif()

add_library(ProwseTk::core ALIAS prowsetk_core)

prowsetk_install_library(prowsetk_core)
