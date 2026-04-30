# All dependencies for lclva.
#
# At M0 we only need: glaze (config), spdlog (logging), doctest (tests).
# Other deps (Boost.Asio, libcurl, PortAudio, soxr, etc.) get added in their
# respective milestones.

find_package(glaze CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)
find_package(doctest CONFIG REQUIRED)
find_package(prometheus-cpp CONFIG REQUIRED COMPONENTS core pull)
find_package(SQLite3 REQUIRED)
find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

# cpp-httplib is vendored as a single header in third_party/cpp-httplib.
# Used for the HTTP control plane — civetweb symbols inside prometheus-cpp
# aren't exported, so we run our own HTTP server. Marked SYSTEM so the
# project's strict warning flags don't apply to this third-party header.
add_library(lclva_httplib INTERFACE)
target_include_directories(lclva_httplib SYSTEM INTERFACE
    ${CMAKE_SOURCE_DIR}/third_party/cpp-httplib)
target_compile_definitions(lclva_httplib INTERFACE
    CPPHTTPLIB_THREAD_POOL_COUNT=4)
