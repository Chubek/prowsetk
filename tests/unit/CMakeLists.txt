prowsetk_gtest(prowsetk_unit_version "prowsetk.unit.version."
    "unit;core" test_version.cpp)

prowsetk_gtest(prowsetk_unit_scrape2postman "prowsetk.unit.scrape2postman."
    "unit;plugin;endpoint-extraction" test_scrape2postman.cpp)
target_link_libraries(prowsetk_unit_scrape2postman PRIVATE prowsetk_scrape2postman_static)
target_compile_definitions(prowsetk_unit_scrape2postman PRIVATE TEST_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}")

prowsetk_gtest(prowsetk_unit_url "prowsetk.unit.url."
    "unit;core" test_url.cpp)

prowsetk_gtest(prowsetk_unit_capability "prowsetk.unit.capability."
    "unit;core" test_capability.cpp)

prowsetk_gtest(prowsetk_unit_events "prowsetk.unit.event."
    "unit;core" test_events.cpp)

prowsetk_gtest(prowsetk_unit_redaction "prowsetk.unit.redaction."
    "unit;core" test_redaction.cpp)

prowsetk_gtest(prowsetk_unit_storage "prowsetk.unit.storage."
    "unit;storage" test_storage.cpp)

prowsetk_gtest(prowsetk_unit_tcb_storage "prowsetk.unit.tcb_storage."
    "unit;storage" test_tcb_storage.cpp)
target_link_libraries(prowsetk_unit_tcb_storage PRIVATE ProwseTk::session)

prowsetk_gtest(prowsetk_unit_encrypted_storage "prowsetk.unit.encrypted_storage."
    "unit;storage" test_encrypted_storage.cpp)
target_link_libraries(prowsetk_unit_encrypted_storage PRIVATE ProwseTk::session)

prowsetk_gtest(prowsetk_unit_dom "prowsetk.unit.document."
    "unit;document" test_dom.cpp)

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

prowsetk_gtest(prowsetk_unit_web_platform "prowsetk.unit.web-platform."
    "unit;javascript" test_web_platform.cpp)

prowsetk_gtest(prowsetk_unit_web_interface "prowsetk.unit.web-interface."
    "unit;web-interface" test_web_interface.cpp)
target_compile_definitions(prowsetk_unit_web_interface PRIVATE
    PROWSETK_TEST_WEB_ROOT="${PROJECT_SOURCE_DIR}/resources/web")

prowsetk_gtest(prowsetk_unit_ir_emitters "prowsetk.unit.ir."
    "unit;document" test_ir_emitters.cpp)
