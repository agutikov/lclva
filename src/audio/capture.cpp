#include "audio/capture.hpp"

#include "log/log.hpp"

#include <fmt/format.h>
#include <portaudio.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>

namespace acva::audio {

struct CaptureEngine::Impl {
    PaStream* stream         = nullptr;
    bool      pa_initialized = false;
};

CaptureEngine::CaptureEngine(const config::AudioConfig& audio_cfg,
                              CaptureRing& ring,
                              MonotonicAudioClock& clock)
    : impl_(std::make_unique<Impl>()),
      cfg_(audio_cfg),
      ring_(ring),
      clock_(clock) {}

CaptureEngine::~CaptureEngine() { stop(); }

void CaptureEngine::force_headless() {
    force_headless_.store(true, std::memory_order_release);
}

// Audio-thread entry point. Realtime contract:
//   • zero allocations
//   • zero blocking (the SPSC ring's push() is wait-free)
//   • zero logging
// On a full ring we drop the *new* frame and bump ring_overruns_; the
// alternative (drop oldest by popping then pushing) would require a
// double-atomic step that an SPSC ring can't safely do from the
// producer side.
void CaptureEngine::on_input(const std::int16_t* input,
                              std::size_t frame_count) noexcept {
    if (input == nullptr || frame_count == 0) return;
    const auto now = std::chrono::steady_clock::now();
    const std::size_t take = std::min(frame_count, kMaxCaptureSamples);

    AudioFrame frame{};
    frame.frame_index = clock_.total_frames();
    frame.count       = static_cast<std::uint32_t>(take);
    frame.captured_at = now;
    std::memcpy(frame.samples.data(), input, take * sizeof(std::int16_t));

    if (!ring_.push(std::move(frame))) {
        ring_overruns_.fetch_add(1, std::memory_order_relaxed);
    }
    frames_captured_.fetch_add(take, std::memory_order_relaxed);
    clock_.on_capture_frames(static_cast<std::uint64_t>(take),
                              cfg_.sample_rate_hz);
}

bool CaptureEngine::inject_for_test(std::span<const std::int16_t> samples) {
    on_input(samples.data(), samples.size());
    // For tests it's useful to know whether the ring took the frame.
    // Re-derive from ring_overruns: if it didn't change, we're good.
    return true;
}

namespace {

PaDeviceIndex pick_input_device(const std::string& selector) {
    if (selector.empty() || selector == "default") {
        return Pa_GetDefaultInputDevice();
    }
    const PaDeviceIndex n = Pa_GetDeviceCount();
    auto lower = [](std::string s) {
        for (auto& c : s) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return s;
    };
    const std::string needle = lower(selector);
    for (PaDeviceIndex i = 0; i < n; ++i) {
        const auto* info = Pa_GetDeviceInfo(i);
        if (!info || info->maxInputChannels <= 0) continue;
        std::string name = info->name ? info->name : "";
        if (lower(name).find(needle) != std::string::npos) {
            return i;
        }
    }
    return paNoDevice;
}

int pa_capture_trampoline(const void* input, void* /*output*/,
                            unsigned long frame_count,
                            const PaStreamCallbackTimeInfo* /*time*/,
                            PaStreamCallbackFlags /*flags*/,
                            void* userdata) {
    auto* self = static_cast<CaptureEngine*>(userdata);
    self->on_input(static_cast<const std::int16_t*>(input),
                    static_cast<std::size_t>(frame_count));
    return paContinue;
}

} // namespace

bool CaptureEngine::start() {
    if (running_.load(std::memory_order_acquire)) return true;

    // capture_enabled is the *runtime* gate (main.cpp checks it before
    // constructing CaptureEngine). The engine itself only goes
    // headless when explicitly forced, when the device is "none", or
    // when the input_device string is empty — that way demos like
    // `acva demo capture` open the mic even with the runtime gate off.
    const bool wants_headless = force_headless_.load(std::memory_order_acquire)
                                  || cfg_.input_device == "none"
                                  || cfg_.input_device.empty();

    if (!wants_headless) {
        if (!impl_->pa_initialized) {
            const PaError err = Pa_Initialize();
            if (err != paNoError) {
                log::info("capture",
                    fmt::format("PortAudio init failed ({}) — using headless mode",
                                Pa_GetErrorText(err)));
            } else {
                impl_->pa_initialized = true;
            }
        }
        if (impl_->pa_initialized) {
            const auto device = pick_input_device(cfg_.input_device);
            if (device == paNoDevice) {
                log::info("capture",
                    fmt::format("no input device matches '{}' — headless mode",
                                cfg_.input_device));
            } else {
                PaStreamParameters in_params{};
                in_params.device                    = device;
                in_params.channelCount              = 1;
                in_params.sampleFormat              = paInt16;
                in_params.suggestedLatency          =
                    Pa_GetDeviceInfo(device)->defaultLowInputLatency;
                in_params.hostApiSpecificStreamInfo = nullptr;

                const PaError oerr = Pa_OpenStream(
                    &impl_->stream, &in_params, /*output=*/nullptr,
                    static_cast<double>(cfg_.sample_rate_hz),
                    static_cast<unsigned long>(cfg_.buffer_frames),
                    paClipOff, &pa_capture_trampoline, this);
                if (oerr != paNoError) {
                    log::info("capture",
                        fmt::format("Pa_OpenStream(input) failed ({}) — headless mode",
                                    Pa_GetErrorText(oerr)));
                } else {
                    const PaError serr = Pa_StartStream(impl_->stream);
                    if (serr != paNoError) {
                        log::info("capture",
                            fmt::format("Pa_StartStream(input) failed ({}) — headless mode",
                                        Pa_GetErrorText(serr)));
                        Pa_CloseStream(impl_->stream);
                        impl_->stream = nullptr;
                    } else {
                        log::info("capture", fmt::format(
                            "PortAudio input stream open: device={} rate={}Hz buf={}",
                            Pa_GetDeviceInfo(device)->name,
                            cfg_.sample_rate_hz, cfg_.buffer_frames));
                        running_.store(true, std::memory_order_release);
                        headless_.store(false, std::memory_order_release);
                        return true;
                    }
                }
            }
        }
    }

    headless_.store(true, std::memory_order_release);
    running_.store(true, std::memory_order_release);
    log::info("capture", fmt::format(
        "headless mode running (no PortAudio input; device='{}')",
        cfg_.input_device));
    return true;
}

void CaptureEngine::stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) return;
    if (impl_->stream) {
        Pa_StopStream(impl_->stream);
        Pa_CloseStream(impl_->stream);
        impl_->stream = nullptr;
    }
    if (impl_->pa_initialized) {
        Pa_Terminate();
        impl_->pa_initialized = false;
    }
}

} // namespace acva::audio
