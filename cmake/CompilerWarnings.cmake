include_guard(GLOBAL)

# prowsetk_set_warnings(<target>)
# Single place that decides the project warning set. Do not duplicate flags in
# individual target definitions.
function(prowsetk_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${target} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wshadow)
    endif()
endfunction()

# Unmodified Replxx 0.0.4 has a pre-C++20 char8_t typedef. GCC 16 also
# diagnoses its UTF decoder's guarded legacy length table as out-of-bounds,
# although isLegalUTF8 rejects those lengths before indexing. Confine these
# compatibility diagnostics to the vendored library, not ProwseTk consumers.
function(prowsetk_replxx_compatibility target)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(${target} PRIVATE -Wno-c++20-compat -Wno-array-bounds)
    endif()
endfunction()
