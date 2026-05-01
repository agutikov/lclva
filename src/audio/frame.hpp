#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>

namespace acva::audio {

// Fixed pre-allocated sample slot per AudioFrame. Sized for the worst-
// case 10 ms PortAudio callback at 48 kHz mono int16 (480 samples). We
// cap at 1024 to absorb the occasional 16 ms / 21 ms slop ALSA emits
// when a host overruns the deadline.
inline constexpr std::size_t kMaxCaptureSamples = 1024;

// One PortAudio callback's worth of int16 mono samples + the frame
// index of the *first* sample in the slot. Lives in the SPSC ring as a
// value type — payload is copied (memcpy) once, in the audio callback,
// from PortAudio's transient buffer into the ring slot.
//
// `count` is the actual number of valid samples in `samples`; it can
// be less than kMaxCaptureSamples if PortAudio hands back a short
// buffer at end-of-stream or when the host requested an unusual frame
// size. Consumers must respect `count` rather than reading the whole
// array.
struct AudioFrame {
    std::uint64_t                          frame_index = 0;
    std::uint32_t                          count       = 0;
    std::chrono::steady_clock::time_point  captured_at{};
    std::array<std::int16_t, kMaxCaptureSamples> samples{};

    [[nodiscard]] std::span<const std::int16_t> view() const noexcept {
        return {samples.data(), count};
    }
};

} // namespace acva::audio
