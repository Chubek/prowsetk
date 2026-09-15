include_guard(GLOBAL)

# prowsetk_add_library(<name> <sources...>)
# Creates a static library, exposes <PROJECT_SOURCE_DIR>/include publicly, and
# applies the shared warning set.
function(prowsetk_add_library name)
    add_library(${name} STATIC ${ARGN})
    target_include_directories(${name} PUBLIC
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:include>")
    target_compile_features(${name} PUBLIC cxx_std_20)
    prowsetk_set_warnings(${name})
endfunction()

# prowsetk_add_executable(<name> <sources...>)
function(prowsetk_add_executable name)
    add_executable(${name} ${ARGN})
    target_include_directories(${name} PRIVATE "${PROJECT_SOURCE_DIR}/include")
    prowsetk_set_warnings(${name})
endfunction()
