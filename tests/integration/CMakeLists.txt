prowsetk_gtest(prowsetk_integration_navigation "prowsetk.integration.network."
    "integration;network" test_navigation.cpp)

prowsetk_gtest(prowsetk_integration_lua "prowsetk.integration.lua."
    "integration;lua" test_lua_binding.cpp)

prowsetk_gtest(prowsetk_integration_lua_extensions "prowsetk.integration.lua."
    "integration;lua" test_lua_extensions.cpp)
target_compile_definitions(prowsetk_integration_lua_extensions PRIVATE
    TEST_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}")
if(PROWSETK_HAVE_PUGIXML)
    target_compile_definitions(prowsetk_integration_lua_extensions PRIVATE
        PROWSETK_HAVE_PUGIXML=1)
endif()

prowsetk_gtest(prowsetk_integration_lua_ir "prowsetk.integration.lua."
    "integration;lua;document" test_lua_ir.cpp)

find_package(Threads REQUIRED)
prowsetk_gtest(prowsetk_integration_web_interface
    "prowsetk.integration.web-interface."
    "integration;web-interface" test_web_interface.cpp)
target_link_libraries(prowsetk_integration_web_interface PRIVATE
    Threads::Threads)

prowsetk_gtest(prowsetk_integration_drivers "prowsetk.integration.driver."
    "integration;lua" test_drivers.cpp)
target_compile_definitions(prowsetk_integration_drivers PRIVATE
    PROWSETK_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    TEST_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}")
if(PROWSETK_HAVE_LUA)
    target_compile_definitions(prowsetk_integration_drivers PRIVATE
        PROWSETK_HAVE_LUA=1)
endif()

if(PROWSETK_BUILD_CLI)
    prowsetk_gtest(prowsetk_integration_cli "prowsetk.integration.cli."
        "integration;lua" test_cli.cpp)
    target_compile_definitions(prowsetk_integration_cli PRIVATE
        PROWSETK_CLI_BIN="$<TARGET_FILE:prowsetk_cli>"
        PROWSETK_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
        TEST_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}")
    if(PROWSETK_HAVE_LUA)
        target_compile_definitions(prowsetk_integration_cli PRIVATE
            PROWSETK_HAVE_LUA=1)
    endif()
    if(PROWSETK_HAVE_TOMLPLUSPLUS)
        target_compile_definitions(prowsetk_integration_cli PRIVATE
            PROWSETK_HAVE_TOMLPLUSPLUS=1)
    endif()
    add_dependencies(prowsetk_integration_cli prowsetk_cli)
endif()
