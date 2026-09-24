# plugins/beacon - Firefox Native Messaging oracle (Flash lifecycle).

if(NOT TARGET ProwseTk::core)
  message(STATUS "beacon: ProwseTk::core not available, skipping")
  return()
endif()

if(PROWSETK_BUILD_TESTS)
    find_program(BEACON_LUA_EXECUTABLE NAMES lua lua5.4)
    if(BEACON_LUA_EXECUTABLE)
        add_test(NAME prowsetk.unit.beacon.lua
            COMMAND "${BEACON_LUA_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_lua.lua"
                "${CMAKE_CURRENT_SOURCE_DIR}/lua/beacon.lua")
        set_tests_properties(prowsetk.unit.beacon.lua PROPERTIES
            LABELS "unit;lua;plugin;beacon" TIMEOUT 10)
    endif()
    find_program(BEACON_NODE_EXECUTABLE node)
    if(BEACON_NODE_EXECUTABLE)
        add_test(NAME prowsetk.integration.beacon.addon
            COMMAND "${BEACON_NODE_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_addon.js"
                "${CMAKE_CURRENT_SOURCE_DIR}/prowsetk-beacon-addon/background.js")
        set_tests_properties(prowsetk.integration.beacon.addon PROPERTIES
            LABELS "integration;plugin;beacon" TIMEOUT 10)
    endif()
endif()

if(PROWSETK_BUILD_TESTS AND UNIX)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME prowsetk.integration.beacon.broker
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_broker.py"
                "$<TARGET_FILE:beacond>" "$<TARGET_FILE:beacon_native_host>"
                "$<TARGET_FILE:beaconctl>")
        set_tests_properties(prowsetk.integration.beacon.broker PROPERTIES
            LABELS "integration;plugin;beacon" TIMEOUT 20
            WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
    endif()
endif()

add_library(prowsetk_beacon_static STATIC src/beacon.cpp)
target_include_directories(prowsetk_beacon_static
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(prowsetk_beacon_static PUBLIC ProwseTk::core)
set_target_properties(prowsetk_beacon_static PROPERTIES
    OUTPUT_NAME "prowsetk_beacon_static" POSITION_INDEPENDENT_CODE ON)
prowsetk_set_warnings(prowsetk_beacon_static)

add_library(prowsetk_beacon SHARED
    src/beacon.cpp
    src/plugin_entry.cpp)
target_include_directories(prowsetk_beacon
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(prowsetk_beacon PRIVATE ProwseTk::core)
set_target_properties(prowsetk_beacon PROPERTIES
    OUTPUT_NAME "prowsetk_beacon"
    PREFIX "lib")
prowsetk_set_warnings(prowsetk_beacon)

# beacond: Flash session daemon (Unix-socket IPC bridge).
add_executable(beacond src/beacond_main.cpp)
target_link_libraries(beacond PRIVATE prowsetk_beacon_static ProwseTk::core)
prowsetk_set_warnings(beacond)

# beacon-native-host: Firefox stdio Native Messaging bridge to beacond.
add_executable(beacon_native_host src/beacon_native_host.cpp)
target_link_libraries(beacon_native_host
    PRIVATE prowsetk_beacon_static ProwseTk::core)
prowsetk_set_warnings(beacon_native_host)

add_executable(beaconctl src/beaconctl.cpp)
target_link_libraries(beaconctl PRIVATE prowsetk_beacon_static ProwseTk::core)
prowsetk_set_warnings(beaconctl)

install(TARGETS prowsetk_beacon
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}/prowsetk/plugins"
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}/prowsetk/plugins")
install(TARGETS beacond beacon_native_host beaconctl
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

install(FILES lua/beacon.lua
    DESTINATION "${CMAKE_INSTALL_DATADIR}/prowsetk/plugins/beacon")
install(FILES include/prowsetk/plugins/beacon.hpp
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/prowsetk/plugins")
install(DIRECTORY prowsetk-beacon-addon/
    DESTINATION "${CMAKE_INSTALL_DATADIR}/prowsetk/beacon/addon")

if(PROWSETK_BUILD_TESTS AND TARGET GTest::gtest_main)
    include(GoogleTest)
    add_executable(prowsetk_plugin_beacon
        tests/test_beacon.cpp)
    target_link_libraries(prowsetk_plugin_beacon
        PRIVATE prowsetk_beacon_static ProwseTk::core GTest::gtest_main)
    target_compile_definitions(prowsetk_plugin_beacon PRIVATE
        BEACON_PLUGIN_PATH="$<TARGET_FILE:prowsetk_beacon>")
    add_dependencies(prowsetk_plugin_beacon prowsetk_beacon)
    prowsetk_set_warnings(prowsetk_plugin_beacon)
    gtest_discover_tests(prowsetk_plugin_beacon
        TEST_PREFIX "prowsetk.plugin.beacon."
        PROPERTIES LABELS "unit;plugin;beacon" TIMEOUT 60
        DISCOVERY_MODE PRE_TEST
        DISCOVERY_TIMEOUT 30)
endif()
