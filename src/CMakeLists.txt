set(PROWSETK_CORE_SOURCES
    prowsetk.cpp
    core/error.cpp
    core/capability.cpp
    core/event.cpp
    core/anti_bot_detection.cpp
    core/cookie_import.cpp
    core/url.cpp
    core/redaction.cpp
    core/storage.cpp
    core/network_client.cpp
    core/flatworm_host.cpp
    core/javascript_runtime.cpp
    core/quickjs_runtime.cpp
    core/web_platform.cpp
    core/wasm_runtime.cpp
    core/plugin_registry.cpp
    core/browser.cpp
    core/lua_runtime.cpp
    core/project_config.cpp
    core/ir_model.cpp
    core/ir_layout.cpp
    core/ir_filter.cpp
    core/ir_vtd.cpp
    core/ir_iml.cpp
    core/ir_registry.cpp
    core/web_interface.cpp
    core/playwright.cpp
    flatworm/html_parser.cpp
    flatworm/css_selector.cpp
    flatworm/dom.cpp
    flatworm/xpath.cpp
    endpoint/endpoint_extraction.cpp)

prowsetk_add_library(prowsetk_core ${PROWSETK_CORE_SOURCES})
set_target_properties(prowsetk_core PROPERTIES EXPORT_NAME core)

target_include_directories(prowsetk_core PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(prowsetk_core PUBLIC ${CMAKE_DL_LIBS})
if(PROWSETK_HAVE_OPENSSL)
    target_compile_definitions(prowsetk_core PRIVATE PROWSETK_HAVE_OPENSSL=1)
    target_link_libraries(prowsetk_core PRIVATE ProwseTk::tls)
endif()
if(PROWSETK_HAVE_LUA)
    target_include_directories(prowsetk_core PRIVATE ${LUA_INCLUDE_DIR})
    target_link_libraries(prowsetk_core PUBLIC ${LUA_LIBRARIES})
    target_compile_definitions(prowsetk_core PRIVATE PROWSETK_HAVE_LUA=1)
endif()

if(PROWSETK_HAVE_QUICKJS)
    target_compile_definitions(prowsetk_core PRIVATE PROWSETK_HAVE_QUICKJS=1)
    target_link_libraries(prowsetk_core PRIVATE ProwseTk::quickjs)
    set_target_properties(qjs PROPERTIES EXPORT_NAME quickjs)
    prowsetk_install_library(qjs)
endif()

if(PROWSETK_HAVE_PUGIXML)
    target_compile_definitions(prowsetk_core PRIVATE PROWSETK_HAVE_PUGIXML=1)
    target_link_libraries(prowsetk_core PRIVATE ProwseTk::pugixml)
    set_target_properties(prowsetk_pugixml PROPERTIES EXPORT_NAME pugixml)
    prowsetk_install_library(prowsetk_pugixml)
endif()

if(PROWSETK_HAVE_TOMLPLUSPLUS)
    target_compile_definitions(prowsetk_core PRIVATE
        PROWSETK_HAVE_TOMLPLUSPLUS=1)
    target_link_libraries(prowsetk_core PRIVATE ProwseTk::tomlplusplus)
    set_target_properties(prowsetk_tomlplusplus PROPERTIES EXPORT_NAME tomlplusplus)
    prowsetk_install_library(prowsetk_tomlplusplus)
endif()

# WASM configuration is threaded through as compile definitions so the disabled
# runtime reports the build configuration honestly (AGENTS.md 4.1, 9).
if(PROWSETK_ENABLE_WASM)
    target_compile_definitions(prowsetk_core PRIVATE PROWSETK_ENABLE_WASM=1)
endif()
if(PROWSETK_ENABLE_WASI)
    target_compile_definitions(prowsetk_core PRIVATE PROWSETK_ENABLE_WASI=1)
endif()
target_compile_definitions(prowsetk_core PRIVATE
    PROWSETK_WASM_RUNTIME_NAME="${PROWSETK_WASM_RUNTIME}")

add_library(ProwseTk::core ALIAS prowsetk_core)

prowsetk_install_library(prowsetk_core)

add_subdirectory(session)

if(EXISTS "${PROJECT_SOURCE_DIR}/plugins/scrape-endpoints/CMakeLists.txt")
    add_subdirectory("${PROJECT_SOURCE_DIR}/plugins/scrape-endpoints" "${CMAKE_BINARY_DIR}/plugins/scrape-endpoints")
endif()
if(EXISTS "${PROJECT_SOURCE_DIR}/plugins/ezlogin/CMakeLists.txt")
    add_subdirectory("${PROJECT_SOURCE_DIR}/plugins/ezlogin" "${CMAKE_BINARY_DIR}/plugins/ezlogin")
endif()
if(EXISTS "${PROJECT_SOURCE_DIR}/plugins/captcha-handler/CMakeLists.txt")
    add_subdirectory("${PROJECT_SOURCE_DIR}/plugins/captcha-handler" "${CMAKE_BINARY_DIR}/plugins/captcha-handler")
endif()
if(EXISTS "${PROJECT_SOURCE_DIR}/plugins/restful-resolver/CMakeLists.txt")
    add_subdirectory("${PROJECT_SOURCE_DIR}/plugins/restful-resolver" "${CMAKE_BINARY_DIR}/plugins/restful-resolver")
endif()
if(EXISTS "${PROJECT_SOURCE_DIR}/plugins/schema-grabber/CMakeLists.txt")
    add_subdirectory("${PROJECT_SOURCE_DIR}/plugins/schema-grabber" "${CMAKE_BINARY_DIR}/plugins/schema-grabber")
endif()
if(EXISTS "${PROJECT_SOURCE_DIR}/plugins/beacon/CMakeLists.txt")
    add_subdirectory("${PROJECT_SOURCE_DIR}/plugins/beacon" "${CMAKE_BINARY_DIR}/plugins/beacon")
endif()

if(PROWSETK_BUILD_CLI)
    add_subdirectory(cli)
endif()
