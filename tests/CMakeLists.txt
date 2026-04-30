add_executable(lclva_tests
    test_main.cpp
    test_bus.cpp
    test_config.cpp
    test_db.cpp
    test_fsm.cpp
    test_llm_client.cpp
    test_manager.cpp
    test_memory_thread.cpp
    test_prompt_builder.cpp
    test_queue.cpp
    test_sse_parser.cpp
    test_recovery.cpp
    test_repository.cpp
    test_sentence_splitter.cpp
)

target_link_libraries(lclva_tests
    PRIVATE
        lclva_core
        lclva_httplib
        doctest::doctest
        lclva_warnings
)

include(${doctest_DIR}/doctest.cmake OPTIONAL RESULT_VARIABLE _doctest_cmake_found)
if(NOT _doctest_cmake_found)
    # Fallback: register the entire binary as one ctest entry. Per-test
    # discovery requires the doctest cmake helper which lives next to the
    # config file.
    add_test(NAME lclva_tests COMMAND lclva_tests)
else()
    doctest_discover_tests(lclva_tests)
endif()
