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
