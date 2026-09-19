include(CTest)
enable_testing()

if(NOT TARGET GTest::gtest_main)
    message(WARNING "GoogleTest not found; ProwseTk test suite disabled")
    return()
endif()

include(GoogleTest)

# prowsetk_gtest(<name> <test-prefix> <labels> <sources...>)
# Builds a GoogleTest executable against ProwseTk::core and registers every
# discovered test case with CTest.
function(prowsetk_gtest name prefix labels)
    prowsetk_add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE ProwseTk::core GTest::gtest GTest::gtest_main)
    gtest_discover_tests(${name}
        TEST_PREFIX "${prefix}"
        PROPERTIES LABELS "${labels}" TIMEOUT 60
        DISCOVERY_MODE PRE_TEST
        DISCOVERY_TIMEOUT 30)
endfunction()

add_subdirectory(plugins)
add_subdirectory(unit)
add_subdirectory(integration)

# Python client compliance suites use the built CLI and are kept as CTest
# cases so Selenium and Playwright regressions are visible in the same test
# dashboard. Optional client packages produce collected, skipped tests when
# they are not installed.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND AND TARGET prowsetk_cli)
    add_test(
        NAME prowsetk.integration.selenium.python
        COMMAND ${Python3_EXECUTABLE} -m pytest -q
            "${PROJECT_SOURCE_DIR}/tests/selenium/test_webdriver_protocol.py"
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
    set_tests_properties(prowsetk.integration.selenium.python PROPERTIES
        LABELS "integration;web-interface"
        TIMEOUT 120
        ENVIRONMENT "PROWSETK_CLI=$<TARGET_FILE:prowsetk_cli>")

    add_test(
        NAME prowsetk.integration.playwright.python
        COMMAND ${Python3_EXECUTABLE} -m pytest -q
            "${PROJECT_SOURCE_DIR}/tests/selenium/test_playwright.py"
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
    set_tests_properties(prowsetk.integration.playwright.python PROPERTIES
        LABELS "integration;web-interface"
        TIMEOUT 120
        ENVIRONMENT "PROWSETK_CLI=$<TARGET_FILE:prowsetk_cli>")
endif()
