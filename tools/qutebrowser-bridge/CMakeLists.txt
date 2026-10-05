# Userscripts are installed together so their standard-library Python helpers
# are resolved beside the executable, independent of source/build-tree paths.
install(PROGRAMS ptk-qute-bridge ptk-qute-send ptk-qute-scrape ptk-qute-marionette
    DESTINATION "${CMAKE_INSTALL_DATADIR}/prowsetk/qutebrowser-bridge")
install(FILES broker.py protocol.py userscript.py README.md
    DESTINATION "${CMAKE_INSTALL_DATADIR}/prowsetk/qutebrowser-bridge")
install(DIRECTORY lua DESTINATION "${CMAKE_INSTALL_DATADIR}/prowsetk/qutebrowser-bridge")
install(DIRECTORY "${PROJECT_SOURCE_DIR}/examples/booking-dotcom-admin-api"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/prowsetk/examples"
    PATTERN "_scraped" EXCLUDE)

if(UNIX AND PROWSETK_HAVE_LUA)
    add_library(prowsetk_qute_ipc MODULE ipc.cpp)
    target_include_directories(prowsetk_qute_ipc PRIVATE ${LUA_INCLUDE_DIR})
    target_link_libraries(prowsetk_qute_ipc PRIVATE ${LUA_LIBRARIES})
    target_compile_features(prowsetk_qute_ipc PRIVATE cxx_std_20)
    set_target_properties(prowsetk_qute_ipc PROPERTIES PREFIX "" OUTPUT_NAME lquteipc)
    prowsetk_set_warnings(prowsetk_qute_ipc)
    install(TARGETS prowsetk_qute_ipc
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}/prowsetk/lua")
endif()
