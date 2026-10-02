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
