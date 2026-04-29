cmake_minimum_required(VERSION 3.25)
project(lclva
    VERSION 0.0.1
    DESCRIPTION "Long Conversation Local Voice Agent"
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

option(LCLVA_BUILD_TESTS "Build unit tests" ON)
option(LCLVA_WERROR "Treat warnings as errors" ON)

include(cmake/Dependencies.cmake)
include(cmake/Warnings.cmake)

add_subdirectory(src)

if(LCLVA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
