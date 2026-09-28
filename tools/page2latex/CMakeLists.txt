prowsetk_add_executable(page2latex page2latex.cpp)
target_compile_features(page2latex PRIVATE cxx_std_20)

install(TARGETS page2latex
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
