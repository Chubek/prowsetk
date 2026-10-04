prowsetk_gtest(prowsetk_unit_version "prowsetk.unit.version."
    "unit;core" test_version.cpp)

if(TARGET ProwseTk::ai_oracle)
    prowsetk_gtest(prowsetk_unit_ai_oracle "prowsetk.unit.ai-oracle."
        "unit;plugin" test_ai_oracle.cpp test_ai_oracle_c.c)
    target_link_libraries(prowsetk_unit_ai_oracle PRIVATE
        ProwseTk::ai_oracle ProwseTk::oracle_json)
    target_compile_definitions(prowsetk_unit_ai_oracle PRIVATE
        AI_ORACLE_PLUGIN_PATH="$<TARGET_FILE:prowsetk_ai_oracle>")
    add_dependencies(prowsetk_unit_ai_oracle prowsetk_ai_oracle)
endif()

prowsetk_gtest(prowsetk_unit_scrape2postman "prowsetk.unit.scrape2postman."
    "unit;plugin;endpoint-extraction" test_scrape2postman.cpp)
target_link_libraries(prowsetk_unit_scrape2postman PRIVATE prowsetk_scrape_endpoints_static)
target_compile_definitions(prowsetk_unit_scrape2postman PRIVATE TEST_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}")

prowsetk_gtest(prowsetk_unit_url "prowsetk.unit.url."
    "unit;core" test_url.cpp)

prowsetk_gtest(prowsetk_unit_capability "prowsetk.unit.capability."
    "unit;core" test_capability.cpp)

prowsetk_gtest(prowsetk_unit_events "prowsetk.unit.event."
    "unit;core" test_events.cpp)

prowsetk_gtest(prowsetk_unit_anti_bot_detection "prowsetk.unit.anti-bot."
    "unit;core;captcha" test_anti_bot_detection.cpp)

prowsetk_gtest(prowsetk_unit_redaction "prowsetk.unit.redaction."
    "unit;core" test_redaction.cpp)

prowsetk_gtest(prowsetk_unit_storage "prowsetk.unit.storage."
    "unit;storage" test_storage.cpp)

prowsetk_gtest(prowsetk_unit_cookie_import "prowsetk.unit.cookie-import."
    "unit;storage" test_cookie_import.cpp)

prowsetk_gtest(prowsetk_unit_tcb_storage "prowsetk.unit.tcb_storage."
    "unit;storage" test_tcb_storage.cpp)
target_link_libraries(prowsetk_unit_tcb_storage PRIVATE ProwseTk::session)

# This suite exercises production-cost key derivation on hundreds of operations;
# ASan/UBSan instrumentation needs a larger finite budget than ordinary units.
prowsetk_gtest(prowsetk_unit_encrypted_storage "prowsetk.unit.encrypted_storage."
    "unit;storage" test_encrypted_storage.cpp TIMEOUT 300)
target_link_libraries(prowsetk_unit_encrypted_storage PRIVATE ProwseTk::session)

prowsetk_gtest(prowsetk_unit_dom "prowsetk.unit.document."
    "unit;document" test_dom.cpp)

prowsetk_gtest(prowsetk_unit_pdql "prowsetk.unit.pdql."
    "unit;document;lua" test_pdql.cpp)
if(PROWSETK_HAVE_PUGIXML)
    target_compile_definitions(prowsetk_unit_pdql PRIVATE PROWSETK_HAVE_PUGIXML=1)
endif()
if(TARGET prowsetk_pagewatch)
    prowsetk_gtest(prowsetk_unit_pagewatch "prowsetk.unit.pagewatch."
        "unit;document;lua;storage" test_pagewatch.cpp)
    target_link_libraries(prowsetk_unit_pagewatch PRIVATE prowsetk_pagewatch)
    target_compile_definitions(prowsetk_unit_pagewatch PRIVATE TEST_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}")
endif()

prowsetk_gtest(prowsetk_unit_css "prowsetk.unit.document.css."
    "unit;document" test_css_selector.cpp)

prowsetk_gtest(prowsetk_unit_xpath "prowsetk.unit.document.xpath."
    "unit;document" test_xpath.cpp)
if(PROWSETK_HAVE_PUGIXML)
    target_compile_definitions(prowsetk_unit_xpath PRIVATE
        PROWSETK_HAVE_PUGIXML=1)
endif()

prowsetk_gtest(prowsetk_unit_network "prowsetk.unit.network."
    "unit;network" test_network.cpp)

find_package(Threads REQUIRED)
prowsetk_gtest(prowsetk_unit_socket_network "prowsetk.unit.network.socket."
    "unit;network" test_socket_network.cpp)
target_link_libraries(prowsetk_unit_socket_network PRIVATE Threads::Threads)

prowsetk_gtest(prowsetk_unit_dom_mutation "prowsetk.unit.document.mutation."
    "unit;document" test_dom_mutation.cpp)

prowsetk_gtest(prowsetk_unit_engine_events "prowsetk.unit.events."
    "unit;core" test_engine_events.cpp)

prowsetk_gtest(prowsetk_unit_project_config "prowsetk.unit.project-config."
    "unit;core" test_project_config.cpp)
if(PROWSETK_HAVE_TOMLPLUSPLUS)
    target_compile_definitions(prowsetk_unit_project_config PRIVATE
        PROWSETK_HAVE_TOMLPLUSPLUS=1)
endif()

prowsetk_gtest(prowsetk_unit_wasm "prowsetk.unit.wasm."
    "unit;wasm" test_wasm.cpp)

prowsetk_gtest(prowsetk_unit_endpoints "prowsetk.unit.endpoint-extraction."
    "unit;endpoint-extraction" test_endpoint_extraction.cpp)

prowsetk_gtest(prowsetk_unit_plugins "prowsetk.unit.plugin."
    "unit;plugin" test_plugin_registry.cpp)
target_compile_definitions(prowsetk_unit_plugins PRIVATE
    TEST_PLUGIN_PATH="$<TARGET_FILE:prowsetk_test_plugin>")
add_dependencies(prowsetk_unit_plugins prowsetk_test_plugin)

prowsetk_gtest(prowsetk_unit_javascript "prowsetk.unit.javascript."
    "unit;javascript" test_javascript_runtime.cpp)

prowsetk_gtest(prowsetk_unit_flatworm_modules "prowsetk.unit.flatworm-module."
    "unit;javascript;core" test_flatworm_modules.cpp)
target_compile_definitions(prowsetk_unit_flatworm_modules PRIVATE
    TEST_MODULE_PATH="$<TARGET_FILE:flatworm_test_module>"
    TEST_BAD_MODULE_PATH="$<TARGET_FILE:flatworm_test_bad_module>"
    TEST_NO_ENTRY_PATH="$<TARGET_FILE:flatworm_test_no_entry>"
    TEST_THROWING_ENTRY_PATH="$<TARGET_FILE:flatworm_test_throwing_entry>")
add_dependencies(prowsetk_unit_flatworm_modules
    flatworm_test_module flatworm_test_bad_module flatworm_test_no_entry flatworm_test_throwing_entry)

if(TARGET flatworm_module_rpc)
    prowsetk_gtest(prowsetk_unit_rpc "prowsetk.unit.rpc."
        "unit;javascript;rpc" test_rpc_module.cpp)
    target_include_directories(prowsetk_unit_rpc PRIVATE
        "${PROJECT_SOURCE_DIR}/flatworm-modules/rpc/src")
    target_link_libraries(prowsetk_unit_rpc PRIVATE flatworm_rpc_impl)
endif()

prowsetk_gtest(prowsetk_unit_flatworm_host "prowsetk.unit.script_host."
    "unit;document;javascript" test_flatworm_host.cpp)

prowsetk_gtest(prowsetk_unit_javascript_platform "prowsetk.unit.javascript_platform."
    "unit;javascript" test_javascript_platform.cpp)

prowsetk_gtest(prowsetk_unit_viewport "prowsetk.unit.viewport."
    "unit;javascript;document" test_viewport.cpp)
prowsetk_gtest(prowsetk_unit_basic_gui "prowsetk.unit.basic-gui."
    "unit;plugin;document" test_basic_gui.cpp)
target_link_libraries(prowsetk_unit_basic_gui PRIVATE
    prowsetk_basic_gui_model prowsetk_basic_gui)

prowsetk_gtest(prowsetk_unit_web_platform "prowsetk.unit.web-platform."
    "unit;javascript" test_web_platform.cpp)

prowsetk_gtest(prowsetk_unit_web_interface "prowsetk.unit.web-interface."
    "unit;web-interface" test_web_interface.cpp)
target_compile_definitions(prowsetk_unit_web_interface PRIVATE
    PROWSETK_TEST_WEB_ROOT="${PROJECT_SOURCE_DIR}/resources/web")

prowsetk_gtest(prowsetk_unit_ir_emitters "prowsetk.unit.ir."
    "unit;document" test_ir_emitters.cpp)

prowsetk_gtest(prowsetk_unit_embedding "prowsetk.unit.embedding."
    "unit;core;document" test_embedding.cpp)

if(PROWSETK_HAVE_OPENSSL AND UNIX)
    prowsetk_gtest(prowsetk_unit_tls_network "prowsetk.unit.network.tls."
        "unit;network" test_tls_network.cpp)
    target_link_libraries(prowsetk_unit_tls_network PRIVATE ProwseTk::tls Threads::Threads)
    target_compile_definitions(prowsetk_unit_tls_network PRIVATE
        TEST_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}")
endif()

prowsetk_gtest(prowsetk_unit_lua_endpoint_options "prowsetk.unit.lua."
    "unit;lua;endpoint-extraction" test_lua_endpoint_options.cpp)

prowsetk_gtest(prowsetk_unit_lua_runtime "prowsetk.unit.lua-runtime."
    "unit;lua" test_lua_runtime.cpp)

prowsetk_gtest(prowsetk_unit_html_parser "prowsetk.unit.html-parser."
    "unit;document" test_html_parser.cpp)

prowsetk_gtest(prowsetk_unit_proxy "prowsetk.unit.proxy." "unit;network" test_proxy.cpp)
if(TARGET prowsetk_spider_static)
    prowsetk_gtest(prowsetk_unit_spider "prowsetk.unit.spider."
        "unit;plugin;storage;lua" test_spider.cpp)
    target_link_libraries(prowsetk_unit_spider PRIVATE prowsetk_spider_static)
    target_compile_definitions(prowsetk_unit_spider PRIVATE
        TEST_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}"
        SPIDER_PLUGIN_PATH="$<TARGET_FILE:prowsetk_spider>")
    add_dependencies(prowsetk_unit_spider prowsetk_spider)
endif()

prowsetk_gtest(prowsetk_unit_opencode_marionette "prowsetk.unit.opencode-marionette."
    "unit;plugin;endpoint-extraction" test_opencode_marionette.cpp)
target_link_libraries(prowsetk_unit_opencode_marionette PRIVATE prowsetk_opencode_marionette_static)
