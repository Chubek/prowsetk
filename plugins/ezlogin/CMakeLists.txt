if(NOT TARGET ProwseTk::core)
  message(STATUS "ezlogin: ProwseTk::core not available, skipping")
  return()
endif()

add_library(prowsetk_ezlogin SHARED src/plugin_entry.cpp)
target_include_directories(prowsetk_ezlogin PRIVATE "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(prowsetk_ezlogin PRIVATE ProwseTk::core)
set_target_properties(prowsetk_ezlogin PROPERTIES OUTPUT_NAME "prowsetk_ezlogin" PREFIX "lib")
prowsetk_set_warnings(prowsetk_ezlogin)

install(TARGETS prowsetk_ezlogin
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}/prowsetk/plugins"
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}/prowsetk/plugins")
install(FILES lua/ezlogin.lua
  DESTINATION "${CMAKE_INSTALL_DATADIR}/prowsetk/plugins/ezlogin")

