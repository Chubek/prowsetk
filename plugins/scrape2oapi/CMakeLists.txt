# plugins/scrape2oapi - native plugin that scrapes suspected internal APIs
# and optionally resolves redirect/API chains, dumping OpenAPI YAML.

if(NOT TARGET ProwseTk::core)
  message(STATUS "scrape2oapi: ProwseTk::core not available, skipping")
  return()
endif()

# Shared-library plugin loaded via PluginRegistry::load_native (dlopen)
add_library(prowsetk_scrape2oapi SHARED
    src/scrape2oapi.cpp
    src/plugin_entry.cpp
)

target_include_directories(prowsetk_scrape2oapi
    PUBLIC  "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${PROJECT_SOURCE_DIR}/include"
)

target_link_libraries(prowsetk_scrape2oapi PRIVATE ProwseTk::core)

# Keep the output name stable for Prowse.toml: plugins/libprowsetk_scrape2oapi.so
set_target_properties(prowsetk_scrape2oapi PROPERTIES
    OUTPUT_NAME "prowsetk_scrape2oapi"
    PREFIX "lib"
)

prowsetk_set_warnings(prowsetk_scrape2oapi)

install(TARGETS prowsetk_scrape2oapi
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}/prowsetk/plugins"
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}/prowsetk/plugins"
)

# Install Lua spec layer
install(FILES lua/scrape2oapi.lua
    DESTINATION "${CMAKE_INSTALL_DATADIR}/prowsetk/plugins/scrape2oapi")

# Static library for unit tests (avoids dlopen)
add_library(prowsetk_scrape2oapi_static STATIC src/scrape2oapi.cpp)
target_include_directories(prowsetk_scrape2oapi_static PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(prowsetk_scrape2oapi_static PUBLIC ProwseTk::core)
set_target_properties(prowsetk_scrape2oapi_static PROPERTIES OUTPUT_NAME "prowsetk_scrape2oapi_static")
prowsetk_set_warnings(prowsetk_scrape2oapi_static)

if(PROWSETK_BUILD_TESTS AND TARGET GTest::gtest_main)
    include(GoogleTest)
    add_executable(prowsetk_plugin_scrape2oapi
        tests/test_scrape2oapi.cpp)
    target_link_libraries(prowsetk_plugin_scrape2oapi
        PRIVATE prowsetk_scrape2oapi_static ProwseTk::core GTest::gtest_main)
    prowsetk_set_warnings(prowsetk_plugin_scrape2oapi)
    gtest_discover_tests(prowsetk_plugin_scrape2oapi
        TEST_PREFIX "prowsetk.plugin.scrape2oapi."
        PROPERTIES LABELS "unit;plugin;endpoint-extraction" TIMEOUT 60
        DISCOVERY_MODE PRE_TEST
        DISCOVERY_TIMEOUT 30)
endif()
