if(NOT PROWSETK_HAVE_HARU)
    message(STATUS "ProwseTk: page2pdf disabled because libHaru is unavailable")
    return()
endif()

prowsetk_add_executable(page2pdf page2pdf.c graphics.c)
target_compile_features(page2pdf PRIVATE c_std_11)
target_link_libraries(page2pdf PRIVATE ProwseTk::haru)

install(TARGETS page2pdf
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
