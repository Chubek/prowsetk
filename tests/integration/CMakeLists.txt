prowsetk_gtest(prowsetk_integration_navigation "prowsetk.integration.network."
    "integration;network" test_navigation.cpp)

if(TARGET ProwseTk::ai_oracle)
    prowsetk_gtest(prowsetk_integration_ai_oracle "prowsetk.integration.ai-oracle."
        "integration;plugin;lua" test_ai_oracle.cpp)
    target_link_libraries(prowsetk_integration_ai_oracle PRIVATE
        ProwseTk::ai_oracle ProwseTk::oracle_json)
    target_compile_definitions(prowsetk_integration_ai_oracle PRIVATE
        AI_ORACLE_PLUGIN_PATH="$<TARGET_FILE:prowsetk_ai_oracle>")
    add_dependencies(prowsetk_integration_ai_oracle prowsetk_ai_oracle)
    if(TARGET prowsetk_ai_oracle_lua)
        target_compile_definitions(prowsetk_integration_ai_oracle PRIVATE
            AI_ORACLE_LUA_PATH="$<TARGET_FILE:prowsetk_ai_oracle_lua>")
        add_dependencies(prowsetk_integration_ai_oracle prowsetk_ai_oracle_lua)
    endif()
endif()

prowsetk_gtest(prowsetk_integration_script_platform
    "prowsetk.integration.script_platform."
    "integration;javascript;document" test_script_platform.cpp)

prowsetk_gtest(prowsetk_integration_flatworm_modules
    "prowsetk.integration.flatworm-module."
    "integration;javascript;document;lua" test_flatworm_modules.cpp)
target_compile_definitions(prowsetk_integration_flatworm_modules PRIVATE
    TEST_MODULE_PATH="$<TARGET_FILE:flatworm_test_module>")
add_dependencies(prowsetk_integration_flatworm_modules flatworm_test_module)

if(TARGET flatworm_module_rpc)
    prowsetk_gtest(prowsetk_integration_rpc "prowsetk.integration.rpc."
        "integration;javascript;document;lua;rpc" test_rpc_module.cpp)
    target_compile_definitions(prowsetk_integration_rpc PRIVATE
        RPC_MODULE_PATH="$<TARGET_FILE:flatworm_module_rpc>")
    add_dependencies(prowsetk_integration_rpc flatworm_module_rpc)
endif()

prowsetk_gtest(prowsetk_integration_scrape2postman "prowsetk.integration.scrape2postman."
    "integration;plugin;endpoint-extraction;lua" test_scrape2postman.cpp)
target_link_libraries(prowsetk_integration_scrape2postman PRIVATE
    prowsetk_scrape_endpoints_static prowsetk_scrape_endpoints)
target_compile_definitions(prowsetk_integration_scrape2postman PRIVATE
    TEST_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}"
    PROWSETK_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    POSTMAN_PLUGIN_PATH="$<TARGET_FILE:prowsetk_scrape_endpoints>")

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

if(TARGET page2latex)
    prowsetk_gtest(prowsetk_integration_page2latex "prowsetk.integration.page2latex."
        "integration;document" test_page2latex.cpp)
    target_compile_definitions(prowsetk_integration_page2latex PRIVATE
        PAGE2LATEX_BIN="$<TARGET_FILE:page2latex>"
        TEST_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}")
    add_dependencies(prowsetk_integration_page2latex page2latex)
    find_program(PROWSETK_XELATEX_EXECUTABLE xelatex)
    if(PROWSETK_XELATEX_EXECUTABLE)
        target_compile_definitions(prowsetk_integration_page2latex PRIVATE
            PAGE2LATEX_ENGINE="${PROWSETK_XELATEX_EXECUTABLE}")
    endif()
endif()

prowsetk_gtest(prowsetk_integration_lua_ir "prowsetk.integration.lua."
    "integration;lua;document" test_lua_ir.cpp)

if(TARGET page2pdf)
    prowsetk_gtest(prowsetk_integration_page2pdf "prowsetk.integration.page2pdf."
        "integration;document" test_page2pdf.cpp)
    target_compile_definitions(prowsetk_integration_page2pdf PRIVATE
        PAGE2PDF_BIN="$<TARGET_FILE:page2pdf>"
        TEST_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}")
    add_dependencies(prowsetk_integration_page2pdf page2pdf)
    if(TARGET prowsetk_cli)
        target_compile_definitions(prowsetk_integration_page2pdf PRIVATE
            PROWSETK_BIN="$<TARGET_FILE:prowsetk_cli>")
        add_dependencies(prowsetk_integration_page2pdf prowsetk_cli)
    endif()
endif()

find_package(Threads REQUIRED)
prowsetk_gtest(prowsetk_integration_web_interface
    "prowsetk.integration.web-interface."
    "integration;web-interface" test_web_interface.cpp)
target_link_libraries(prowsetk_integration_web_interface PRIVATE
    Threads::Threads)

prowsetk_gtest(prowsetk_integration_webdriver "prowsetk.integration.webdriver."
    "integration;web-interface" test_webdriver.cpp)

prowsetk_gtest(prowsetk_integration_cdp "prowsetk.integration.cdp."
    "integration;web-interface;playwright" test_cdp.cpp)
target_include_directories(prowsetk_integration_cdp PRIVATE
    "${PROJECT_SOURCE_DIR}/src/core")
target_link_libraries(prowsetk_integration_cdp PRIVATE Threads::Threads)

prowsetk_gtest(prowsetk_integration_drivers "prowsetk.integration.driver."
    "integration;lua" test_drivers.cpp)
target_compile_definitions(prowsetk_integration_drivers PRIVATE
    PROWSETK_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    TEST_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}")

if(TARGET crawler)
    prowsetk_gtest(prowsetk_integration_crawler "prowsetk.integration.crawler."
        "integration;lua;document;endpoint-extraction" test_crawler.cpp)
    target_link_libraries(prowsetk_integration_crawler PRIVATE prowsetk_automation)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME prowsetk.integration.crawler.command
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_crawler_cli.py"
                "$<TARGET_FILE:crawler>" "${PROJECT_SOURCE_DIR}")
        set_tests_properties(prowsetk.integration.crawler.command PROPERTIES
            LABELS "integration;lua;document;endpoint-extraction" TIMEOUT 60
            WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
    endif()
endif()
if(TARGET pgwatchd)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME prowsetk.integration.pagewatch.daemon
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_pagewatch_daemon.py"
                "$<TARGET_FILE:pgwatchd>" "$<TARGET_FILE:pgwatchctl>")
        set_tests_properties(prowsetk.integration.pagewatch.daemon PROPERTIES
            LABELS "integration;lua;document;storage" TIMEOUT 60
            WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
    endif()
endif()
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
    target_sources(prowsetk_integration_cli PRIVATE
        "${PROJECT_SOURCE_DIR}/src/cli/serialize_resources.cpp")
    target_include_directories(prowsetk_integration_cli PRIVATE
        "${PROJECT_SOURCE_DIR}/src/cli")
    if(UNIX AND PROWSETK_HAVE_LUA AND PROWSETK_HAVE_TOMLPLUSPLUS)
        find_package(Python3 COMPONENTS Interpreter QUIET)
        if(Python3_Interpreter_FOUND)
            add_test(NAME prowsetk.integration.cli.booking_assistant_browser
                COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_booking_assistant_browser.py"
                    "$<TARGET_FILE:prowsetk_cli>" "${PROJECT_SOURCE_DIR}")
            set_tests_properties(prowsetk.integration.cli.booking_assistant_browser PROPERTIES
                LABELS "integration;lua;document" TIMEOUT 60
                WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
        endif()
    endif()
endif()
if(TARGET ptkspiderd AND PROWSETK_HAVE_LUA AND PROWSETK_HAVE_QUICKJS)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME prowsetk.integration.spider.daemon
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_spider_daemon.py"
                "$<TARGET_FILE:ptkspiderd>" "$<TARGET_FILE:ptkspiderctl>")
        set_tests_properties(prowsetk.integration.spider.daemon PROPERTIES
            LABELS "integration;plugin;lua;storage" TIMEOUT 60
            WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
    endif()
endif()

prowsetk_gtest(prowsetk_integration_opencode_marionette "prowsetk.integration.opencode-marionette."
    "integration;plugin;javascript;endpoint-extraction" test_opencode_marionette.cpp)
target_link_libraries(prowsetk_integration_opencode_marionette PRIVATE prowsetk_opencode_marionette_static)
target_compile_definitions(prowsetk_integration_opencode_marionette PRIVATE
    PROWSETK_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    MARIONETTE_PLUGIN_PATH="$<TARGET_FILE:prowsetk_opencode_marionette>")
add_dependencies(prowsetk_integration_opencode_marionette prowsetk_opencode_marionette)
if(TARGET ptk_opencode_marionette)
    add_test(NAME prowsetk.integration.opencode-marionette.cli.usage COMMAND ptk_opencode_marionette)
    set_tests_properties(prowsetk.integration.opencode-marionette.cli.usage PROPERTIES
        LABELS "integration;plugin;endpoint-extraction" TIMEOUT 10 WILL_FAIL TRUE)
    if(UNIX AND PROWSETK_HAVE_LUA AND PROWSETK_HAVE_TOMLPLUSPLUS AND PROWSETK_HAVE_QUICKJS)
        find_package(Python3 COMPONENTS Interpreter QUIET)
        if(Python3_Interpreter_FOUND)
            add_test(NAME prowsetk.integration.opencode-marionette.booking-launcher
                COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_booking_marionette.py"
                    "$<TARGET_FILE:ptk_opencode_marionette>" "${PROJECT_SOURCE_DIR}")
            set_tests_properties(prowsetk.integration.opencode-marionette.booking-launcher PROPERTIES
                LABELS "integration;plugin;lua;javascript;endpoint-extraction" TIMEOUT 60
                WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
        endif()
    endif()
endif()
