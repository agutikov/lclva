# Core library — everything except main(). Tests link against this.
add_library(lclva_core STATIC
    config/config.cpp
    dialogue/fsm.cpp
    dialogue/sentence_splitter.cpp
    dialogue/turn.cpp
    event/bus.cpp
    event/event.cpp
    http/server.cpp
    llm/prompt_builder.cpp
    log/log.cpp
    memory/db.cpp
    memory/memory_thread.cpp
    memory/recovery.cpp
    memory/repository.cpp
    metrics/registry.cpp
    pipeline/fake_driver.cpp
)

target_include_directories(lclva_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(lclva_core
    PUBLIC
        glaze::glaze
        spdlog::spdlog
        fmt::fmt
        prometheus-cpp::core
        prometheus-cpp::pull
        SQLite::SQLite3
        CURL::libcurl
        Threads::Threads
    PRIVATE
        lclva_httplib
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
