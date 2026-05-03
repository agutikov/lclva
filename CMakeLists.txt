cmake_minimum_required(VERSION 3.25)
project(acva
    VERSION 0.0.1
    DESCRIPTION "Autonomous Conversational Voice Agent"
    LANGUAGES CXX
)

# C++23 baseline. The CLAUDE.md / project_design.md §16 lock specifically
# forbids Boost.Cobalt and C++23 modules — but plain C++23 STL features
# (std::expected, deducing this, std::print) are allowed. Glaze 7.x requires
# C++23 anyway, so this is implicit through our config dependency.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(ACVA_BUILD_TESTS "Build unit tests" ON)
option(ACVA_WERROR "Treat warnings as errors" ON)
# Per-TU compile-time tracing (clang ≥ 9 / gcc ≥ 14). Adds
# `-ftime-trace`; each compiled .cpp emits a sibling .json file in
# `_build/<preset>/...` that Chrome's chrome://tracing or speedscope
# can render as a flamegraph. ninja's per-file wall-time is also
# available via `scripts/build-times.sh` without enabling this.
option(ACVA_TIME_TRACE "Emit per-TU compile time traces (-ftime-trace)" OFF)

include(cmake/Dependencies.cmake)
include(cmake/Warnings.cmake)

if(ACVA_TIME_TRACE)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        add_compile_options(-ftime-trace)
        message(STATUS "ACVA_TIME_TRACE: -ftime-trace enabled; "
                       "look for *.json next to each .o under "
                       "${CMAKE_BINARY_DIR}/")
    else()
        message(WARNING "ACVA_TIME_TRACE: ignored — compiler "
                        "${CMAKE_CXX_COMPILER_ID} doesn't support -ftime-trace")
    endif()
endif()

add_subdirectory(src)

if(ACVA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
