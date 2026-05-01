# All dependencies for acva.
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

# M3 — audio output. PortAudio drives the realtime playback callback;
# soxr handles 22.05 (Piper) → 48 kHz (device), reused in M4 for the
# 48 → 16 kHz capture path. Both ship as system pkg-config modules on
# Manjaro/Arch and Ubuntu — no vendoring.
find_package(PkgConfig REQUIRED)
pkg_check_modules(portaudio REQUIRED IMPORTED_TARGET portaudio-2.0)
pkg_check_modules(soxr      REQUIRED IMPORTED_TARGET soxr)

# M4 — Silero VAD via ONNX Runtime. Optional; if the package isn't
# installed the VAD wrapper is omitted and the M4 capture demos fall
# back to a no-op probability of 0. The voice agent itself logs a
# warning at startup and disables the dialogue path's capture trigger.
find_package(onnxruntime CONFIG QUIET)
if(onnxruntime_FOUND)
    set(ACVA_HAVE_ONNXRUNTIME TRUE)
else()
    set(ACVA_HAVE_ONNXRUNTIME FALSE)
    message(STATUS "onnxruntime not found — Silero VAD will be a no-op stub")
endif()

# cpp-httplib is vendored as a single header in third_party/cpp-httplib.
# Used for the HTTP control plane — civetweb symbols inside prometheus-cpp
# aren't exported, so we run our own HTTP server. Marked SYSTEM so the
# project's strict warning flags don't apply to this third-party header.
add_library(acva_httplib INTERFACE)
target_include_directories(acva_httplib SYSTEM INTERFACE
    ${CMAKE_SOURCE_DIR}/third_party/cpp-httplib)
target_compile_definitions(acva_httplib INTERFACE
    CPPHTTPLIB_THREAD_POOL_COUNT=4)
