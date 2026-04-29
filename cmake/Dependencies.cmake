# All dependencies for lclva.
#
# At M0 we only need: glaze (config), spdlog (logging), doctest (tests).
# Other deps (Boost.Asio, libcurl, PortAudio, soxr, etc.) get added in their
# respective milestones.

find_package(glaze CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)
find_package(doctest CONFIG REQUIRED)
find_package(Threads REQUIRED)
