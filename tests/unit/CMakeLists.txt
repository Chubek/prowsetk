prowsetk_gtest(prowsetk_unit_version "prowsetk.unit.version."
    "unit;core" test_version.cpp)

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
