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
    target_link_libraries(${name} PRIVATE ProwseTk::core GTest::gtest_main)
    gtest_discover_tests(${name}
        TEST_PREFIX "${prefix}"
        PROPERTIES LABELS "${labels}" TIMEOUT 60
        DISCOVERY_TIMEOUT 30)
endfunction()

add_subdirectory(plugins)
add_subdirectory(unit)
add_subdirectory(integration)
