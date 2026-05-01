add_executable(acva_tests
    test_main.cpp
    test_bus.cpp
    test_config.cpp
    test_db.cpp
    test_fsm.cpp
    test_json_sink.cpp
    test_keep_alive.cpp
    test_llm_client.cpp
    test_manager.cpp
    test_memory_thread.cpp
    test_paths.cpp
    test_piper_client.cpp
    test_playback_engine.cpp
    test_playback_queue.cpp
    test_probe.cpp
    test_prompt_builder.cpp
    test_service_monitor.cpp
    test_supervisor.cpp
    test_queue.cpp
    test_sse_parser.cpp
    test_summarizer.cpp
    test_recovery.cpp
    test_repository.cpp
    test_resampler.cpp
    test_sentence_splitter.cpp
    test_tts_bridge.cpp
    test_turn_writer.cpp
)

target_link_libraries(acva_tests
    PRIVATE
        acva_core
        acva_httplib
        doctest::doctest
        acva_warnings
)

include(${doctest_DIR}/doctest.cmake OPTIONAL RESULT_VARIABLE _doctest_cmake_found)
if(NOT _doctest_cmake_found)
    # Fallback: register the entire binary as one ctest entry. Per-test
    # discovery requires the doctest cmake helper which lives next to the
    # config file.
    add_test(NAME acva_tests COMMAND acva_tests)
else()
    doctest_discover_tests(acva_tests)
endif()
