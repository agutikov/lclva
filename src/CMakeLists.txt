# Core library — everything except main(). Tests link against this.
add_library(lclva_core STATIC
    config/config.cpp
    log/log.cpp
)

target_include_directories(lclva_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(lclva_core
    PUBLIC
        glaze::glaze
        spdlog::spdlog
        fmt::fmt
        Threads::Threads
    PRIVATE
        lclva_warnings
)

# Main executable
add_executable(lclva main.cpp)

target_link_libraries(lclva
    PRIVATE
        lclva_core
        lclva_warnings
)

set_target_properties(lclva PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
