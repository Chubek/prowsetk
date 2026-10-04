# Expose preset-level sanitizer flags to nanobind's target-based runtime
# discovery. Keep runtime preloading scoped to Python, not the C++ test suite.
function(prowsetk_python_sanitizers target environment_out)
    string(TOUPPER "${CMAKE_BUILD_TYPE}" build_type)
    separate_arguments(flags NATIVE_COMMAND
        "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${build_type}}")
    set(sanitizers "")
    foreach(flag IN LISTS flags)
        if(flag MATCHES "^-fsanitize=")
            target_compile_options(${target} PRIVATE "${flag}")
            string(REGEX REPLACE "^-fsanitize=" "" values "${flag}")
            string(REPLACE "," ";" values "${values}")
            list(APPEND sanitizers ${values})
        endif()
    endforeach()
    set(environment "")
    if(UNIX)
        nanobind_sanitizer_preload_env(environment ${target})
        # Linux compiler-rt uses architecture-qualified names. Nanobind's
        # unqualified Clang query can return an empty LD_PRELOAD directive.
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND NOT APPLE
                AND environment STREQUAL "LD_PRELOAD=")
            set(runtime "")
            if("address" IN_LIST sanitizers)
                # The ASan shared runtime also supplies UBSan's handlers.
                set(runtime "asan")
            elseif("thread" IN_LIST sanitizers)
                set(runtime "tsan")
            elseif("undefined" IN_LIST sanitizers)
                set(runtime "ubsan_standalone")
            endif()
            if(runtime)
                execute_process(
                    COMMAND "${CMAKE_CXX_COMPILER}" ${flags} -print-target-triple
                    RESULT_VARIABLE triple_status OUTPUT_VARIABLE triple
                    OUTPUT_STRIP_TRAILING_WHITESPACE)
                string(REGEX REPLACE "-.*$" "" architecture "${triple}")
                execute_process(
                    COMMAND "${CMAKE_CXX_COMPILER}" ${flags}
                        "-print-file-name=libclang_rt.${runtime}-${architecture}.so"
                    RESULT_VARIABLE runtime_status OUTPUT_VARIABLE runtime_path
                    OUTPUT_STRIP_TRAILING_WHITESPACE)
                if(NOT triple_status EQUAL 0 OR NOT runtime_status EQUAL 0
                        OR NOT EXISTS "${runtime_path}")
                    message(FATAL_ERROR "Python sanitizer runtime is unavailable for this Clang target")
                endif()
                set(environment "LD_PRELOAD=${runtime_path}")
            endif()
        endif()
        if(environment MATCHES "asan")
            # The unsanitized Python interpreter retains process-global objects.
            # C++ tests retain their normal leak checking.
            list(APPEND environment "ASAN_OPTIONS=detect_leaks=0")
        endif()
    endif()
    set(${environment_out} "${environment}" PARENT_SCOPE)
endfunction()
