# Two test binaries:
#
#   acva_unit_tests        — pure unit tests, no external deps. Stands
#                             up in-process httplib servers when it
#                             needs HTTP fixtures; never reads
#                             on-disk model files; never opens real
#                             audio devices.
#
#   acva_integration_tests — tests that consume real on-disk assets
#                             (Silero VAD model, future: Whisper
#                             models, Piper voices) or live local
#                             services. Must run env-var-free on the
#                             dev workstation by resolving paths via
#                             config::resolve_data_path's XDG defaults.
#                             Tests still gracefully `* doctest::skip`
#                             when the dep isn't present so the suite
#                             is runnable for someone who hasn't fully
#                             provisioned the env.
#
# `run_tests.sh` runs unit by default for fast iteration;
# `run_integration_tests.sh` runs the integration suite.

# ----- shared helpers -----
set(_acva_test_main test_main.cpp)

# ----- unit suite -----
add_executable(acva_unit_tests
    ${_acva_test_main}
    test_apm.cpp
    test_audio_pipeline.cpp
    test_bus.cpp
    test_config.cpp
    test_db.cpp
    test_endpointer.cpp
    test_fsm.cpp
    test_half_duplex_gate.cpp
    test_json_sink.cpp
    test_keep_alive.cpp
    test_llm_client.cpp
    test_loopback.cpp
    test_manager.cpp
    test_memory_thread.cpp
    test_paths.cpp
    test_playback_engine.cpp
    test_playback_queue.cpp
    test_probe.cpp
    test_prompt_builder.cpp
    test_service_monitor.cpp
    test_spsc_ring.cpp
    test_supervisor.cpp
    test_system_aec.cpp
    test_queue.cpp
    test_sse_parser.cpp
    test_summarizer.cpp
    test_realtime_envelope.cpp
    test_realtime_event_dispatch.cpp
    test_recovery.cpp
    test_repository.cpp
    test_resampler.cpp
    test_sentence_splitter.cpp
    test_tts_bridge.cpp
    test_turn_writer.cpp
    test_utterance.cpp
)

target_link_libraries(acva_unit_tests
    PRIVATE
        acva_core
        acva_httplib
        doctest::doctest
        acva_warnings
)

# ----- integration suite -----
set(_acva_integration_sources
    ${_acva_test_main}
    test_speaches_smoke.cpp
    test_speaches_wedge.cpp
    test_vad.cpp
)

if(ACVA_HAVE_LIBDATACHANNEL)
    list(APPEND _acva_integration_sources test_speaches_realtime_smoke.cpp)
endif()

add_executable(acva_integration_tests ${_acva_integration_sources})

target_link_libraries(acva_integration_tests
    PRIVATE
        acva_core
        acva_httplib
        doctest::doctest
        acva_warnings
)

if(ACVA_HAVE_LIBDATACHANNEL)
    target_link_libraries(acva_integration_tests
        PRIVATE LibDataChannel::LibDataChannel)
endif()

# ----- ctest registration -----
include(${doctest_DIR}/doctest.cmake OPTIONAL RESULT_VARIABLE _doctest_cmake_found)
if(NOT _doctest_cmake_found)
    add_test(NAME acva_unit_tests        COMMAND acva_unit_tests)
    add_test(NAME acva_integration_tests COMMAND acva_integration_tests)
else()
    doctest_discover_tests(acva_unit_tests)
    doctest_discover_tests(acva_integration_tests)
endif()
