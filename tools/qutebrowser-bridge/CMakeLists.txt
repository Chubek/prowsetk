# Userscripts are installed together so their standard-library Python helpers
# are resolved beside the executable, independent of source/build-tree paths.
install(PROGRAMS ptk-qute-bridge ptk-qute-send ptk-qute-scrape ptk-qute-marionette
    DESTINATION "${CMAKE_INSTALL_DATADIR}/prowsetk/qutebrowser-bridge")
install(FILES broker.py protocol.py bulk.py userscript.py README.md
    DESTINATION "${CMAKE_INSTALL_DATADIR}/prowsetk/qutebrowser-bridge")
install(DIRECTORY lua DESTINATION "${CMAKE_INSTALL_DATADIR}/prowsetk/qutebrowser-bridge")
install(DIRECTORY "${PROJECT_SOURCE_DIR}/examples/booking-dotcom-admin-api"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/prowsetk/examples"
    PATTERN "_scraped" EXCLUDE)

if(UNIX AND PROWSETK_HAVE_LUA)
    add_library(prowsetk_qute_ipc MODULE ipc.cpp fifo.cpp)
    target_include_directories(prowsetk_qute_ipc PRIVATE ${LUA_INCLUDE_DIR})
    target_link_libraries(prowsetk_qute_ipc PRIVATE ${LUA_LIBRARIES})
    target_compile_features(prowsetk_qute_ipc PRIVATE cxx_std_20)
    set_target_properties(prowsetk_qute_ipc PROPERTIES PREFIX "" OUTPUT_NAME lquteipc)
    prowsetk_set_warnings(prowsetk_qute_ipc)
    install(TARGETS prowsetk_qute_ipc
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}/prowsetk/lua")
    if(PROWSETK_BUILD_QUTE_REPL AND TARGET ProwseTk::replxx)
        prowsetk_add_executable(ptk_qute_repl repl.cpp)
        set_target_properties(ptk_qute_repl PROPERTIES OUTPUT_NAME ptk-qute-repl)
        target_link_libraries(ptk_qute_repl PRIVATE ProwseTk::core ProwseTk::replxx)
        target_compile_definitions(ptk_qute_repl PRIVATE
            QUTE_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}"
            QUTE_IPC_BUILD_PATH="$<TARGET_FILE:prowsetk_qute_ipc>"
            QUTE_INSTALL_SHARE="${CMAKE_INSTALL_FULL_DATADIR}/prowsetk/qutebrowser-bridge"
            QUTE_INSTALL_IPC="${CMAKE_INSTALL_FULL_LIBDIR}/prowsetk/lua/lquteipc.so")
        add_dependencies(ptk_qute_repl prowsetk_qute_ipc)
        install(TARGETS ptk_qute_repl RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
    endif()
endif()
