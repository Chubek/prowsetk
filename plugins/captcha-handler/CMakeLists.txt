if(NOT TARGET ProwseTk::core)
  message(STATUS "captcha-handler: ProwseTk::core not available, skipping")
  return()
endif()

add_library(prowsetk_captcha_handler_static STATIC
    src/captcha_handler.cpp)
target_include_directories(prowsetk_captcha_handler_static
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(prowsetk_captcha_handler_static PUBLIC ProwseTk::core)
set_target_properties(prowsetk_captcha_handler_static PROPERTIES
    OUTPUT_NAME "prowsetk_captcha_handler_static"
    POSITION_INDEPENDENT_CODE ON)
prowsetk_set_warnings(prowsetk_captcha_handler_static)

add_library(prowsetk_captcha_handler SHARED
    src/plugin_entry.cpp)
target_link_libraries(prowsetk_captcha_handler
    PRIVATE prowsetk_captcha_handler_static)
set_target_properties(prowsetk_captcha_handler PROPERTIES
    OUTPUT_NAME "prowsetk_captcha_handler"
    PREFIX "lib")
prowsetk_set_warnings(prowsetk_captcha_handler)

install(TARGETS prowsetk_captcha_handler
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}/prowsetk/plugins"
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}/prowsetk/plugins")

install(FILES include/prowsetk/plugins/captcha_handler.hpp
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/prowsetk/plugins")

if(PROWSETK_BUILD_TESTS AND TARGET GTest::gtest_main)
    include(GoogleTest)
    add_executable(prowsetk_plugin_captcha_handler
        tests/test_captcha_handler.cpp)
    target_link_libraries(prowsetk_plugin_captcha_handler
        PRIVATE prowsetk_captcha_handler_static ProwseTk::core GTest::gtest_main)
    target_compile_definitions(prowsetk_plugin_captcha_handler PRIVATE
        CAPTCHA_HANDLER_PLUGIN_PATH="$<TARGET_FILE:prowsetk_captcha_handler>")
    add_dependencies(prowsetk_plugin_captcha_handler prowsetk_captcha_handler)
    prowsetk_set_warnings(prowsetk_plugin_captcha_handler)
    gtest_discover_tests(prowsetk_plugin_captcha_handler
        TEST_PREFIX "prowsetk.plugin.captcha-handler."
        PROPERTIES LABELS "unit;plugin;captcha" TIMEOUT 60
        DISCOVERY_MODE PRE_TEST
        DISCOVERY_TIMEOUT 30)
endif()
