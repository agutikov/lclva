# Core library — everything except main(). Tests link against this.
add_library(acva_core STATIC
    audio/resampler.cpp
    config/config.cpp
    config/paths.cpp
    dialogue/fsm.cpp
    dialogue/manager.cpp
    dialogue/sentence_splitter.cpp
    dialogue/turn.cpp
    dialogue/turn_writer.cpp
    event/bus.cpp
    event/event.cpp
    http/server.cpp
    llm/client.cpp
    llm/prompt_builder.cpp
    llm/sse_parser.cpp
    log/json_sink.cpp
    log/log.cpp
    memory/db.cpp
    memory/memory_thread.cpp
    memory/recovery.cpp
    memory/repository.cpp
    memory/summarizer.cpp
    metrics/registry.cpp
    pipeline/fake_driver.cpp
    playback/engine.cpp
    playback/queue.cpp
    supervisor/keep_alive.cpp
    supervisor/probe.cpp
    supervisor/service.cpp
    supervisor/supervisor.cpp
    tts/piper_client.cpp
)

target_include_directories(acva_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(acva_core
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
        PkgConfig::portaudio
        PkgConfig::soxr
        acva_httplib
        acva_warnings
)

# Main executable
add_executable(acva main.cpp)

target_link_libraries(acva
    PRIVATE
        acva_core
        acva_warnings
)

set_target_properties(acva PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
