include_guard(GLOBAL)

# Declarative dependency wiring. This is the single place that names third-party
# packages. Locate each dependency with find_package(... CONFIG) and, when it is
# not found, fall back to vendored sources under third_party/. Every dependency
# must be exposed as a ProwseTk::<name> imported target.
#
# Example:
#   find_package(fmt CONFIG QUIET)
#   if(NOT fmt_FOUND)
#       add_subdirectory("${PROJECT_SOURCE_DIR}/third_party/fmt" EXCLUDE_FROM_ALL)
#   endif()
#   add_library(ProwseTk::fmt ALIAS fmt::fmt)
