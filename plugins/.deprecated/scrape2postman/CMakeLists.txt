prowsetk_add_library(prowsetk_scrape2postman_static src/scrape2postman.cpp)
target_include_directories(prowsetk_scrape2postman_static PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(prowsetk_scrape2postman_static PUBLIC prowsetk_scrape2oapi_static)

add_library(prowsetk_scrape2postman SHARED src/plugin_entry.cpp)
target_link_libraries(prowsetk_scrape2postman PRIVATE prowsetk_scrape2postman_static)
prowsetk_set_warnings(prowsetk_scrape2postman)
set_target_properties(prowsetk_scrape2postman PROPERTIES PREFIX "lib")

install(TARGETS prowsetk_scrape2postman
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}/prowsetk/plugins"
    RUNTIME DESTINATION "${CMAKE_INSTALL_LIBDIR}/prowsetk/plugins")
install(FILES lua/scrape2postman.lua
    DESTINATION "${CMAKE_INSTALL_DATADIR}/prowsetk/plugins/scrape2postman")
install(FILES include/prowsetk/plugins/scrape2postman.hpp
    "${PROJECT_SOURCE_DIR}/plugins/scrape2oapi/include/prowsetk/plugins/scrape2oapi.hpp"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/prowsetk/plugins")
