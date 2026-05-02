# Core library — everything except main(). Tests link against this.
add_library(acva_core STATIC
    audio/capture.cpp
    audio/clock.cpp
    audio/endpointer.cpp
    audio/pipeline.cpp
    audio/resampler.cpp
    audio/utterance.cpp
    audio/vad.cpp
    config/config.cpp
    config/paths.cpp
    demos/capture.cpp
    demos/chat.cpp
    demos/demo.cpp
    demos/fsm.cpp
    demos/health.cpp
    demos/llm.cpp
    demos/loopback.cpp
    demos/stt.cpp
    demos/tone.cpp
    demos/transcribe.cpp
    demos/tts.cpp
    dialogue/fsm.cpp
    dialogue/manager.cpp
    dialogue/sentence_splitter.cpp
    dialogue/tts_bridge.cpp
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
    stt/openai_stt_client.cpp
    stt/realtime_envelope.cpp
    stt/realtime_event_dispatch.cpp
    stt/realtime_stt_client.cpp
    supervisor/keep_alive.cpp
    supervisor/probe.cpp
    supervisor/service.cpp
    supervisor/supervisor.cpp
    tts/openai_tts_client.cpp
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

# M4 — link ONNX Runtime when present and define the gate macro so
# audio/vad.cpp picks the real implementation. Without it the file
# compiles to a stub that throws on construction.
if(ACVA_HAVE_ONNXRUNTIME)
    target_link_libraries(acva_core PRIVATE onnxruntime::onnxruntime)
    target_compile_definitions(acva_core PRIVATE ACVA_HAVE_ONNXRUNTIME=1)
endif()

# M5 — link libdatachannel when present and define the gate macro so
# stt/realtime_stt_client.cpp picks the real implementation. Without
# it the file compiles to a stub whose start() returns false.
if(ACVA_HAVE_LIBDATACHANNEL)
    target_link_libraries(acva_core PRIVATE LibDataChannel::LibDataChannel)
    target_compile_definitions(acva_core PRIVATE ACVA_HAVE_LIBDATACHANNEL=1)
endif()

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
