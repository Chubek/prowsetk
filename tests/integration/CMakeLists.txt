prowsetk_add_executable(prowsetk_integration_smoke
    test_smoke_integration.cpp)
target_link_libraries(prowsetk_integration_smoke PRIVATE ProwseTk::core)

add_test(NAME prowsetk.integration.smoke COMMAND prowsetk_integration_smoke)
set_tests_properties(prowsetk.integration.smoke PROPERTIES
    TIMEOUT 60
    LABELS "integration;core")
