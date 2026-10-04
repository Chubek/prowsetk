# plugins/opencode-bridge - native ProwseTk <-> OpenCode bridge.
# Conforms to ProwseTk-Plugin.h ABI v2 and exposes the `lopencode` Lua module.
# All HTTP goes through the host NetworkClient; no new production dependencies.

if(NOT TARGET ProwseTk::core)
  message(STATUS "opencode-bridge: ProwseTk::core not available, skipping")
  return()
endif()

add_library(prowsetk_opencode_bridge_static STATIC
    src/opencode_client.cpp
    src/dom_serializer.cpp)
target_include_directories(prowsetk_opencode_bridge_static
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(prowsetk_opencode_bridge_static PUBLIC ProwseTk::core)
set_target_properties(prowsetk_opencode_bridge_static PROPERTIES
    OUTPUT_NAME "prowsetk_opencode_bridge_static" POSITION_INDEPENDENT_CODE ON)
prowsetk_set_warnings(prowsetk_opencode_bridge_static)

add_library(prowsetk_opencode_bridge SHARED
    src/bridge_core.cpp
    src/opencode_client.cpp
    src/dom_serializer.cpp)
target_include_directories(prowsetk_opencode_bridge
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(prowsetk_opencode_bridge PRIVATE ProwseTk::core)
set_target_properties(prowsetk_opencode_bridge PROPERTIES
    OUTPUT_NAME "prowsetk_opencode_bridge"
    PREFIX "lib")
prowsetk_set_warnings(prowsetk_opencode_bridge)

install(TARGETS prowsetk_opencode_bridge
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}/prowsetk/plugins"
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}/prowsetk/plugins")
install(FILES include/opencode_bridge.hpp
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/prowsetk/plugins")
install(FILES README.md
    DESTINATION "${CMAKE_INSTALL_DATADIR}/prowsetk/plugins/opencode-bridge")
install(FILES lua/lopencode.lua
    DESTINATION "${CMAKE_INSTALL_DATADIR}/prowsetk/plugins/opencode-bridge")

if(PROWSETK_HAVE_LUA)
    add_library(prowsetk_lopencode MODULE
        src/lua_bindings.cpp
        src/opencode_client.cpp
        src/dom_serializer.cpp)
    target_include_directories(prowsetk_lopencode PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include" ${LUA_INCLUDE_DIR})
    target_link_libraries(prowsetk_lopencode PRIVATE
        ProwseTk::core ${LUA_LIBRARIES})
    set_target_properties(prowsetk_lopencode PROPERTIES
        PREFIX "" OUTPUT_NAME lopencode)
    prowsetk_set_warnings(prowsetk_lopencode)
    install(TARGETS prowsetk_lopencode
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}/prowsetk/lua")
endif()

if(PROWSETK_BUILD_TESTS AND TARGET GTest::gtest_main)
    include(GoogleTest)
    add_executable(prowsetk_plugin_opencode_bridge_client
        tests/test_client.cpp)
    target_link_libraries(prowsetk_plugin_opencode_bridge_client
        PRIVATE prowsetk_opencode_bridge_static ProwseTk::core GTest::gtest_main)
    prowsetk_set_warnings(prowsetk_plugin_opencode_bridge_client)
    gtest_discover_tests(prowsetk_plugin_opencode_bridge_client
        TEST_PREFIX "prowsetk.plugin.opencode-bridge."
        PROPERTIES LABELS "unit;plugin;lua" TIMEOUT 60
        DISCOVERY_MODE PRE_TEST
        DISCOVERY_TIMEOUT 30)

    if(PROWSETK_HAVE_LUA AND TARGET prowsetk_lopencode)
        add_executable(prowsetk_plugin_opencode_bridge_lua
            tests/test_lua_api.cpp)
        target_link_libraries(prowsetk_plugin_opencode_bridge_lua
            PRIVATE prowsetk_opencode_bridge_static ProwseTk::core GTest::gtest_main)
        target_compile_definitions(prowsetk_plugin_opencode_bridge_lua PRIVATE
            LOPENCODE_LUA_PATH="$<TARGET_FILE:prowsetk_lopencode>")
        add_dependencies(prowsetk_plugin_opencode_bridge_lua prowsetk_lopencode)
        prowsetk_set_warnings(prowsetk_plugin_opencode_bridge_lua)
        gtest_discover_tests(prowsetk_plugin_opencode_bridge_lua
            TEST_PREFIX "prowsetk.plugin.opencode-bridge.lua."
            PROPERTIES LABELS "unit;plugin;lua" TIMEOUT 60
            DISCOVERY_MODE PRE_TEST
            DISCOVERY_TIMEOUT 30)
    endif()
endif()
