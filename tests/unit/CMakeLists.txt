prowsetk_add_executable(prowsetk_unit_smoke
    test_smoke.cpp)
target_link_libraries(prowsetk_unit_smoke PRIVATE ProwseTk::core)

add_test(NAME prowsetk.unit.smoke COMMAND prowsetk_unit_smoke)
set_tests_properties(prowsetk.unit.smoke PROPERTIES
    TIMEOUT 30
    LABELS "unit;core")
