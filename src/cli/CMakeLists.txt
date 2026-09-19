prowsetk_add_executable(prowsetk_cli prowsetk_main.cpp)
target_link_libraries(prowsetk_cli PRIVATE ProwseTk::core)
target_include_directories(prowsetk_cli PRIVATE "${PROJECT_SOURCE_DIR}/src/core")

# Point the CLI at the repository's static web UI so `prowsetk serve` finds
# index.html/app.js/style.css when run from a build tree. The --web-root flag
# overrides this at runtime.
set_property(TARGET prowsetk_cli PROPERTY OUTPUT_NAME prowsetk)
target_compile_definitions(prowsetk_cli PRIVATE
    PROWSETK_WEB_ROOT="${PROJECT_SOURCE_DIR}/resources/web")

install(TARGETS prowsetk_cli
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(DIRECTORY "${PROJECT_SOURCE_DIR}/resources/web/"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/prowsetk/web")
install(FILES "${PROJECT_SOURCE_DIR}/man/man1/prowsetk.1"
    DESTINATION "${CMAKE_INSTALL_MANDIR}/man1")
install(FILES "${PROJECT_SOURCE_DIR}/man/man5/Prowse.toml.5"
    DESTINATION "${CMAKE_INSTALL_MANDIR}/man5")
