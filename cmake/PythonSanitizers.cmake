# Expose preset-level sanitizer flags to nanobind's target-based runtime
# discovery. Keep runtime preloading scoped to Python, not the C++ test suite.
function(prowsetk_python_sanitizers target environment_out)
    string(TOUPPER "${CMAKE_BUILD_TYPE}" build_type)
    separate_arguments(flags NATIVE_COMMAND
        "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${build_type}}")
    foreach(flag IN LISTS flags)
        if(flag MATCHES "^-fsanitize=")
            target_compile_options(${target} PRIVATE "${flag}")
        endif()
    endforeach()
    set(environment "")
    if(UNIX)
        nanobind_sanitizer_preload_env(environment ${target})
        if(environment MATCHES "asan")
            # The unsanitized Python interpreter retains process-global objects.
            # C++ tests retain their normal leak checking.
            list(APPEND environment "ASAN_OPTIONS=detect_leaks=0")
        endif()
    endif()
    set(${environment_out} "${environment}" PARENT_SCOPE)
endfunction()
