# plugins/scrape-endpoints - unified endpoint discovery exporter for OpenAPI and Postman.

if(NOT TARGET ProwseTk::core)
  message(STATUS "scrape-endpoints: ProwseTk::core not available, skipping")
  return()
endif()

add_library(prowsetk_scrape_endpoints_static STATIC src/scrape_endpoints.cpp)
target_include_directories(prowsetk_scrape_endpoints_static
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(prowsetk_scrape_endpoints_static PUBLIC ProwseTk::core)
set_target_properties(prowsetk_scrape_endpoints_static PROPERTIES
    OUTPUT_NAME "prowsetk_scrape_endpoints_static" POSITION_INDEPENDENT_CODE ON)
prowsetk_set_warnings(prowsetk_scrape_endpoints_static)

add_library(prowsetk_scrape_endpoints SHARED
    src/scrape_endpoints.cpp
    src/plugin_entry.cpp)
target_include_directories(prowsetk_scrape_endpoints
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(prowsetk_scrape_endpoints PRIVATE ProwseTk::core)
set_target_properties(prowsetk_scrape_endpoints PROPERTIES
    OUTPUT_NAME "prowsetk_scrape_endpoints"
    PREFIX "lib")
prowsetk_set_warnings(prowsetk_scrape_endpoints)

# Compatibility target names for existing in-tree tests and downstream builds.
add_library(prowsetk_scrape2oapi_static ALIAS prowsetk_scrape_endpoints_static)
add_library(prowsetk_scrape2postman_static ALIAS prowsetk_scrape_endpoints_static)
add_library(prowsetk_scrape2oapi ALIAS prowsetk_scrape_endpoints)
add_library(prowsetk_scrape2postman ALIAS prowsetk_scrape_endpoints)

install(TARGETS prowsetk_scrape_endpoints
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}/prowsetk/plugins"
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}/prowsetk/plugins")

install(FILES lua/scrape_endpoints.lua
    DESTINATION "${CMAKE_INSTALL_DATADIR}/prowsetk/plugins/scrape-endpoints")
install(FILES include/prowsetk/plugins/scrape_endpoints.hpp
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/prowsetk/plugins")

if(PROWSETK_BUILD_TESTS AND TARGET GTest::gtest_main)
    include(GoogleTest)
    add_executable(prowsetk_plugin_scrape_endpoints
        tests/test_scrape_endpoints.cpp)
    target_link_libraries(prowsetk_plugin_scrape_endpoints
        PRIVATE prowsetk_scrape_endpoints_static ProwseTk::core GTest::gtest_main)
    prowsetk_set_warnings(prowsetk_plugin_scrape_endpoints)
    gtest_discover_tests(prowsetk_plugin_scrape_endpoints
        TEST_PREFIX "prowsetk.plugin.scrape-endpoints."
        PROPERTIES LABELS "unit;plugin;endpoint-extraction" TIMEOUT 60
        DISCOVERY_MODE PRE_TEST
        DISCOVERY_TIMEOUT 30)
endif()
