# Native plugin fixture loaded by the plugin registry tests. It is a shared
# object so the registry exercises the real dlopen/dlsym path.
add_library(prowsetk_test_plugin SHARED test_plugin.c)

target_include_directories(prowsetk_test_plugin PRIVATE
    "${PROJECT_SOURCE_DIR}/include")
set_target_properties(prowsetk_test_plugin PROPERTIES
    PREFIX ""
    OUTPUT_NAME "prowsetk_test_plugin")
prowsetk_set_warnings(prowsetk_test_plugin)
