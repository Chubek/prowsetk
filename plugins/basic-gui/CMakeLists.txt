prowsetk_add_library(prowsetk_basic_gui_model
    src/availability.cpp src/controller.cpp src/projection.cpp)
target_include_directories(prowsetk_basic_gui_model PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_link_libraries(prowsetk_basic_gui_model PUBLIC ProwseTk::core)
set_target_properties(prowsetk_basic_gui_model PROPERTIES EXPORT_NAME basic_gui_model)
if(PROWSETK_BUILD_BASIC_GUI)
    target_compile_definitions(prowsetk_basic_gui_model PUBLIC PROWSETK_HAVE_BASIC_GUI=1)
endif()
add_library(ProwseTk::basic_gui_model ALIAS prowsetk_basic_gui_model)
prowsetk_install_library(prowsetk_basic_gui_model)

add_library(prowsetk_basic_gui SHARED src/plugin_entry.cpp src/marionette.cpp src/opencode.cpp)
target_link_libraries(prowsetk_basic_gui PUBLIC ProwseTk::core PRIVATE prowsetk_basic_gui_model prowsetk_opencode_bridge_static prowsetk_opencode_marionette_static)
target_include_directories(prowsetk_basic_gui PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
prowsetk_set_warnings(prowsetk_basic_gui)
if(PROWSETK_BUILD_BASIC_GUI)
    target_sources(prowsetk_basic_gui PRIVATE src/viewer.cpp)
    target_link_libraries(prowsetk_basic_gui PRIVATE ProwseTk::fltk)
    target_compile_definitions(prowsetk_basic_gui PRIVATE PROWSETK_HAVE_BASIC_GUI=1)
    prowsetk_add_executable(ptk_basic_gui src/main.cpp)
    target_link_libraries(ptk_basic_gui PRIVATE prowsetk_basic_gui prowsetk_basic_gui_model)
    set_target_properties(ptk_basic_gui PROPERTIES OUTPUT_NAME ptk-basic-gui)
    install(TARGETS ptk_basic_gui RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
else()
    target_sources(prowsetk_basic_gui PRIVATE src/headless.cpp)
endif()
install(TARGETS prowsetk_basic_gui
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}/prowsetk/plugins"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(FILES include/prowsetk/plugins/basic_gui.hpp
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/prowsetk/plugins")
install(FILES README.md example.html marionette.lua Prowse.toml
    DESTINATION "${CMAKE_INSTALL_DATADIR}/prowsetk/plugins/basic-gui")
