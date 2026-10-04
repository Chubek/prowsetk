prowsetk_add_library(flatworm_rpc_impl
    src/json.cpp
    src/json_parser.cpp
    src/json_encoder.cpp
    src/protocol.cpp
    src/module.cpp)
target_include_directories(flatworm_rpc_impl PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
set_target_properties(flatworm_rpc_impl PROPERTIES
    CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)

prowsetk_add_flatworm_module(flatworm_module_rpc src/entry.cpp)
target_link_libraries(flatworm_module_rpc PRIVATE flatworm_rpc_impl)
set_target_properties(flatworm_module_rpc PROPERTIES
    OUTPUT_NAME flatworm_rpc
    CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
install(TARGETS flatworm_module_rpc
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}/prowsetk/flatworm-modules"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}/prowsetk/flatworm-modules")

if(PROWSETK_BUILD_EXAMPLES)
    prowsetk_add_executable(flatworm_rpc_example examples/embed.cpp)
    target_link_libraries(flatworm_rpc_example PRIVATE ProwseTk::core)
    add_dependencies(flatworm_rpc_example flatworm_module_rpc)
endif()
