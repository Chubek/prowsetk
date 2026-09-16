include_guard(GLOBAL)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# prowsetk_add_library(<name> <sources...>)
# Creates a static library, exposes <PROJECT_SOURCE_DIR>/include publicly, and
# applies the shared warning set.
function(prowsetk_add_library name)
    add_library(${name} STATIC ${ARGN})
    set_target_properties(${name} PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_include_directories(${name} PUBLIC
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
    target_compile_features(${name} PUBLIC cxx_std_20)
    prowsetk_set_warnings(${name})
endfunction()

# prowsetk_add_executable(<name> <sources...>)
function(prowsetk_add_executable name)
    add_executable(${name} ${ARGN})
    target_include_directories(${name} PRIVATE "${PROJECT_SOURCE_DIR}/include")
    prowsetk_set_warnings(${name})
endfunction()

# prowsetk_add_test(<name> <sources...>)
# Creates a test executable and registers it with CTest. Callers add LABELS,
# TIMEOUT, and library links afterwards.
function(prowsetk_add_test name)
    prowsetk_add_executable(${name} ${ARGN})
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES TIMEOUT 60)
endfunction()

# prowsetk_install_library(<target>)
# Records a library for installation and export.
function(prowsetk_install_library target)
    install(TARGETS ${target}
        EXPORT ProwseTkTargets
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
        INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
endfunction()

# prowsetk_export_package()
# Installs headers and generates the ProwseTkConfig.cmake / Targets package.
function(prowsetk_export_package)
    install(DIRECTORY "${PROJECT_SOURCE_DIR}/include/"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

    install(EXPORT ProwseTkTargets
        FILE ProwseTkTargets.cmake
        NAMESPACE ProwseTk::
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/ProwseTk")

    configure_package_config_file(
        "${PROJECT_SOURCE_DIR}/cmake/ProwseTkConfig.cmake.in"
        "${PROJECT_BINARY_DIR}/ProwseTkConfig.cmake"
        INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/ProwseTk")

    write_basic_package_version_file(
        "${PROJECT_BINARY_DIR}/ProwseTkConfigVersion.cmake"
        VERSION "${PROJECT_VERSION}"
        COMPATIBILITY SameMajorVersion)

    install(FILES
        "${PROJECT_BINARY_DIR}/ProwseTkConfig.cmake"
        "${PROJECT_BINARY_DIR}/ProwseTkConfigVersion.cmake"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/ProwseTk")
endfunction()
