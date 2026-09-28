cmake_minimum_required(VERSION 3.25)

# pyprowsetk - Python bindings for ProwseTk using nanobind
# This directory is included optionally from the top-level CMakeLists.txt
# when PROWSETK_BUILD_PYTHON is ON and Python is available.

option(PROWSETK_BUILD_PYTHON "Build pyprowsetk Python bindings" ON)

if(NOT PROWSETK_BUILD_PYTHON)
    message(STATUS "pyprowsetk: PROWSETK_BUILD_PYTHON=OFF, skipping Python bindings")
    return()
endif()

find_package(Python 3.9 COMPONENTS Interpreter Development.Module QUIET)

if(NOT Python_FOUND)
    message(STATUS "pyprowsetk: Python 3.9+ Development.Module not found; skipping Python bindings")
    message(STATUS "pyprowsetk: install python3-dev and retry to build the bindings")
    return()
endif()

message(STATUS "pyprowsetk: building Python bindings with Python ${Python_VERSION} at ${Python_EXECUTABLE}")

# Bring in nanobind - vendored under third_party/nanobind
set(NB_USE_SUBMODULE_DEPS ON CACHE BOOL "" FORCE)
if(NOT TARGET nanobind-static)
    add_subdirectory("${PROJECT_SOURCE_DIR}/third_party/nanobind"
                     "${CMAKE_BINARY_DIR}/third_party/nanobind-py")
endif()

nanobind_add_module(pyprowsetk_core
    src/pyprowsetk.cpp
)

target_link_libraries(pyprowsetk_core PRIVATE ProwseTk::core)

# Ensure PIC and C++20
target_compile_features(pyprowsetk_core PRIVATE cxx_std_20)
set_target_properties(pyprowsetk_core PROPERTIES
    OUTPUT_NAME "_core"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/pyprowsetk"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/pyprowsetk"
)

# Install the Python package: the compiled extension + pure-python wrappers
install(TARGETS pyprowsetk_core
    LIBRARY DESTINATION "pyprowsetk"
    ARCHIVE DESTINATION "pyprowsetk"
)

install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/pyprowsetk/"
    DESTINATION "pyprowsetk"
    FILES_MATCHING PATTERN "*.py"
)

install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/pyproject.toml"
    DESTINATION "."
    OPTIONAL
)

# Each preset owns its package: sanitizer and development builds must not
# overwrite one another or import a module copied into the source tree.
configure_file(pyprowsetk/__init__.py pyprowsetk/__init__.py COPYONLY)
include("${PROJECT_SOURCE_DIR}/cmake/PythonSanitizers.cmake")
prowsetk_python_sanitizers(pyprowsetk_core PROWSETK_PYTHON_SANITIZER_ENV)

# Stub generation (PEP 561) - best-effort
if(Python_Interpreter_FOUND)
    nanobind_add_stub(pyprowsetk_stub
        MODULE pyprowsetk._core
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/pyprowsetk/_core.pyi"
        PYTHON_PATH "${CMAKE_CURRENT_BINARY_DIR}"
        DEPENDS pyprowsetk_core
        VERBOSE
    )
endif()

# Register pytest suite with CTest when pytest is available
if(Python_Interpreter_FOUND)
    find_program(PYTEST_EXECUTABLE pytest)
    if(PYTEST_EXECUTABLE)
        add_test(NAME pyprowsetk.unit.python
            COMMAND ${Python_EXECUTABLE} -m pytest -q "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_pyprowsetk.py"
            WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
        set_tests_properties(pyprowsetk.unit.python PROPERTIES
            LABELS "unit;python"
            TIMEOUT 60
            ENVIRONMENT "PYTHONPATH=${CMAKE_CURRENT_BINARY_DIR};${PROWSETK_PYTHON_SANITIZER_ENV}")
    endif()
endif()
