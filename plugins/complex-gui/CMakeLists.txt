prowsetk_add_library(prowsetk_complex_gui_model src/controller.cpp)
target_include_directories(prowsetk_complex_gui_model PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_link_libraries(prowsetk_complex_gui_model PUBLIC ProwseTk::core)
set_target_properties(prowsetk_complex_gui_model PROPERTIES EXPORT_NAME complex_gui_model)
add_library(ProwseTk::complex_gui_model ALIAS prowsetk_complex_gui_model)
prowsetk_install_library(prowsetk_complex_gui_model)

add_library(prowsetk_complex_gui SHARED src/plugin_entry.cpp)
target_link_libraries(prowsetk_complex_gui PUBLIC prowsetk_complex_gui_model)
prowsetk_set_warnings(prowsetk_complex_gui)
if(PROWSETK_BUILD_COMPLEX_GUI)
    target_sources(prowsetk_complex_gui PRIVATE src/viewer.cpp)
    target_link_libraries(prowsetk_complex_gui PRIVATE ProwseTk::fltk)
    target_compile_definitions(prowsetk_complex_gui PRIVATE PROWSETK_HAVE_COMPLEX_GUI=1)
else()
    target_sources(prowsetk_complex_gui PRIVATE src/headless.cpp)
endif()
add_library(ProwseTk::complex_gui ALIAS prowsetk_complex_gui)
set_target_properties(prowsetk_complex_gui PROPERTIES EXPORT_NAME complex_gui)
install(TARGETS prowsetk_complex_gui EXPORT ProwseTkTargets
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}/prowsetk/plugins"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(FILES include/prowsetk/plugins/complex_gui.hpp
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/prowsetk/plugins")
install(FILES README.md Prowse.toml DESTINATION "${CMAKE_INSTALL_DATADIR}/prowsetk/plugins/complex-gui")
