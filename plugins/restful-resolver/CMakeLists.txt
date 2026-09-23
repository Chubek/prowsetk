# plugins/restful-resolver - native plugin that resolves scraped
# endpoints iteratively until a full RESTful API surface (GET+POST) is found.

if(NOT TARGET ProwseTk::core)
  message(STATUS "restful-resolver: ProwseTk::core not available, skipping")
  return()
endif()

add_library(prowsetk_restful_resolver SHARED
    src/restful_resolver.cpp
    src/plugin_entry.cpp
)

target_include_directories(prowsetk_restful_resolver
    PUBLIC  "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${PROJECT_SOURCE_DIR}/include"
)

target_link_libraries(prowsetk_restful_resolver PRIVATE ProwseTk::core)

# Keep the output name stable for Prowse.toml: plugins/librestful_resolver.so
# is exposed as libprowsetk_restful_resolver.so for consistency with siblings.
set_target_properties(prowsetk_restful_resolver PROPERTIES
    OUTPUT_NAME "prowsetk_restful_resolver"
    PREFIX "lib"
)

prowsetk_set_warnings(prowsetk_restful_resolver)

install(TARGETS prowsetk_restful_resolver
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}/prowsetk/plugins"
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}/prowsetk/plugins"
)

install(FILES lua/restful_resolver.lua
    DESTINATION "${CMAKE_INSTALL_DATADIR}/prowsetk/plugins/restful-resolver")

install(FILES include/prowsetk/plugins/restful_resolver.hpp
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/prowsetk/plugins")

# Static library for unit tests (avoids dlopen)
add_library(prowsetk_restful_resolver_static STATIC src/restful_resolver.cpp)
target_include_directories(prowsetk_restful_resolver_static PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(prowsetk_restful_resolver_static PUBLIC ProwseTk::core)
set_target_properties(prowsetk_restful_resolver_static PROPERTIES
    OUTPUT_NAME "prowsetk_restful_resolver_static" POSITION_INDEPENDENT_CODE ON)
prowsetk_set_warnings(prowsetk_restful_resolver_static)

if(PROWSETK_BUILD_TESTS AND TARGET GTest::gtest_main)
    include(GoogleTest)
    add_executable(prowsetk_plugin_restful_resolver
        tests/test_restful_resolver.cpp)
    target_link_libraries(prowsetk_plugin_restful_resolver
        PRIVATE prowsetk_restful_resolver_static ProwseTk::core GTest::gtest_main)
    target_compile_definitions(prowsetk_plugin_restful_resolver PRIVATE
        RESTFUL_RESOLVER_PLUGIN_PATH="$<TARGET_FILE:prowsetk_restful_resolver>")
    add_dependencies(prowsetk_plugin_restful_resolver prowsetk_restful_resolver)
    prowsetk_set_warnings(prowsetk_plugin_restful_resolver)
    gtest_discover_tests(prowsetk_plugin_restful_resolver
        TEST_PREFIX "prowsetk.plugin.restful-resolver."
        PROPERTIES LABELS "unit;plugin;endpoint-extraction" TIMEOUT 60
        DISCOVERY_MODE PRE_TEST
        DISCOVERY_TIMEOUT 30)
endif()
