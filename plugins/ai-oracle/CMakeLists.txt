if(NOT TARGET ProwseTk::openaipp OR NOT TARGET ProwseTk::oracle_json)
    message(STATUS "ai-oracle: OpenAIpp/JSON headers unavailable, skipping")
    return()
endif()

prowsetk_add_library(prowsetk_ai_oracle_static
    src/ai_oracle.cpp src/c_api.cpp)
target_include_directories(prowsetk_ai_oracle_static PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_link_libraries(prowsetk_ai_oracle_static PUBLIC ProwseTk::core PRIVATE
    "$<BUILD_INTERFACE:ProwseTk::openaipp>"
    "$<BUILD_INTERFACE:ProwseTk::oracle_json>")
set_target_properties(prowsetk_ai_oracle_static PROPERTIES EXPORT_NAME ai_oracle)
add_library(ProwseTk::ai_oracle ALIAS prowsetk_ai_oracle_static)
prowsetk_install_library(prowsetk_ai_oracle_static)

add_library(prowsetk_ai_oracle SHARED src/plugin_entry.cpp src/c_api.cpp)
target_link_libraries(prowsetk_ai_oracle PRIVATE prowsetk_ai_oracle_static)
prowsetk_set_warnings(prowsetk_ai_oracle)
install(TARGETS prowsetk_ai_oracle
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}/prowsetk/plugins"
    RUNTIME DESTINATION "${CMAKE_INSTALL_LIBDIR}/prowsetk/plugins")
install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(FILES README.md DESTINATION "${CMAKE_INSTALL_DATADIR}/prowsetk/plugins/ai-oracle")

if(PROWSETK_HAVE_LUA)
    add_library(prowsetk_ai_oracle_lua MODULE src/lua_module.cpp)
    target_include_directories(prowsetk_ai_oracle_lua PRIVATE ${LUA_INCLUDE_DIR})
    target_link_libraries(prowsetk_ai_oracle_lua PRIVATE
        prowsetk_ai_oracle_static ${LUA_LIBRARIES})
    set_target_properties(prowsetk_ai_oracle_lua PROPERTIES
        PREFIX "" OUTPUT_NAME ai_oracle)
    prowsetk_set_warnings(prowsetk_ai_oracle_lua)
    install(TARGETS prowsetk_ai_oracle_lua
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}/prowsetk/lua")
endif()
