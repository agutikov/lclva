#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace acva::audio {

// Pack `samples` into a RIFF/WAVE container. Mono int16 PCM at the
// given sample rate. Used by the STT client to wrap captured audio
// for /v1/audio/transcriptions, and by the M6B aec-record demo to
// dump the original / raw / cleaned signals to disk for offline
// analysis.
[[nodiscard]] std::string make_wav(std::span<const std::int16_t> samples,
                                    std::uint32_t sample_rate_hz);

// Convenience: write `samples` to `path` as a WAV file. Returns true
// on success. Does not throw — diagnostics on failure are written via
// the caller's preferred channel.
[[nodiscard]] bool write_wav_file(std::string_view path,
                                   std::span<const std::int16_t> samples,
                                   std::uint32_t sample_rate_hz);

// Read a mono int16 PCM WAV file. Returns the samples; sets
// `sample_rate_hz` to the file's rate. Returns an empty vector and
// leaves `sample_rate_hz` untouched on parse failure or non-mono /
// non-int16 inputs. Used by tests; not on any hot path.
[[nodiscard]] std::vector<std::int16_t>
read_wav_file(std::string_view path, std::uint32_t& sample_rate_hz);

} // namespace acva::audio
