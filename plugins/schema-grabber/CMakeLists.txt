# plugins/schema-grabber - native plugin that reverse-engineers
# request/response schemas and URL parameters for scraped endpoints.

if(NOT TARGET ProwseTk::core)
  message(STATUS "schema-grabber: ProwseTk::core not available, skipping")
  return()
endif()

add_library(prowsetk_schema_grabber_static STATIC src/schema_grabber.cpp)
target_include_directories(prowsetk_schema_grabber_static
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(prowsetk_schema_grabber_static PUBLIC ProwseTk::core)
set_target_properties(prowsetk_schema_grabber_static PROPERTIES
    OUTPUT_NAME "prowsetk_schema_grabber_static" POSITION_INDEPENDENT_CODE ON)
prowsetk_set_warnings(prowsetk_schema_grabber_static)

add_library(prowsetk_schema_grabber SHARED
    src/schema_grabber.cpp
    src/plugin_entry.cpp)
target_include_directories(prowsetk_schema_grabber
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(prowsetk_schema_grabber PRIVATE ProwseTk::core)
set_target_properties(prowsetk_schema_grabber PROPERTIES
    OUTPUT_NAME "prowsetk_schema_grabber"
    PREFIX "lib")
prowsetk_set_warnings(prowsetk_schema_grabber)

install(TARGETS prowsetk_schema_grabber
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}/prowsetk/plugins"
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}/prowsetk/plugins")

install(FILES lua/schema_grabber.lua
    DESTINATION "${CMAKE_INSTALL_DATADIR}/prowsetk/plugins/schema-grabber")
install(FILES include/prowsetk/plugins/schema_grabber.hpp
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/prowsetk/plugins")

if(PROWSETK_BUILD_TESTS AND TARGET GTest::gtest_main)
    include(GoogleTest)
    add_executable(prowsetk_plugin_schema_grabber
        tests/test_schema_grabber.cpp)
    target_link_libraries(prowsetk_plugin_schema_grabber
        PRIVATE prowsetk_schema_grabber_static ProwseTk::core GTest::gtest_main)
    if(TARGET prowsetk_scrape_endpoints_static)
        target_link_libraries(prowsetk_plugin_schema_grabber
            PRIVATE prowsetk_scrape_endpoints_static)
        target_compile_definitions(prowsetk_plugin_schema_grabber PRIVATE
            SCHEMA_GRABBER_HAVE_SCRAPE_ENDPOINTS=1)
    endif()
    target_compile_definitions(prowsetk_plugin_schema_grabber PRIVATE
        SCHEMA_GRABBER_PLUGIN_PATH="$<TARGET_FILE:prowsetk_schema_grabber>")
    add_dependencies(prowsetk_plugin_schema_grabber prowsetk_schema_grabber)
    prowsetk_set_warnings(prowsetk_plugin_schema_grabber)
    gtest_discover_tests(prowsetk_plugin_schema_grabber
        TEST_PREFIX "prowsetk.plugin.schema-grabber."
        PROPERTIES LABELS "unit;plugin;endpoint-extraction" TIMEOUT 60
        DISCOVERY_MODE PRE_TEST
        DISCOVERY_TIMEOUT 30)
endif()
