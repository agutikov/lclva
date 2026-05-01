#include "playback/engine.hpp"

#include "log/log.hpp"

#include <fmt/format.h>
#include <portaudio.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <utility>

namespace acva::playback {

namespace {

// Cap on the postbox queue: at 10 ms chunks and N=64 in-flight, the
// publisher only ever sees a handful of finished events between wake-
// ups. 256 is generous and bounds memory.
constexpr std::size_t kPostboxCapacity = 256;

} // namespace

struct PlaybackEngine::Impl {
    // PortAudio handle. nullptr in headless mode or before start().
    PaStream* stream = nullptr;
    bool      pa_initialized = false;

    // Per-cb state. Audio-thread-only — never touched from elsewhere.
    AudioChunk current{};
    std::size_t pos = 0;

    // Cross-thread postbox: audio thread → publisher thread. DropOldest
    // so a stuck publisher can't backpressure the audio callback.
    event::BoundedQueue<event::PlaybackFinished> postbox;

    std::thread publisher;
    std::atomic<bool> publisher_stop{false};

    // Headless ticker (used when device == "none" or PortAudio fails).
    // The cv lets stop() wake the ticker out of its sleep_for so
    // teardown is bounded by the cv hand-off, not the tick interval.
    std::thread headless_thread;
    std::mutex  headless_mu;
    std::condition_variable headless_cv;

    Impl()
        : postbox(kPostboxCapacity, event::OverflowPolicy::DropOldest) {}
};

PlaybackEngine::PlaybackEngine(const config::AudioConfig& audio_cfg,
                                 const config::PlaybackConfig& playback_cfg,
                                 PlaybackQueue& queue,
                                 event::EventBus& bus,
                                 ActiveTurnFn active_turn)
    : impl_(std::make_unique<Impl>()),
      cfg_(audio_cfg),
      playback_cfg_(playback_cfg),
      queue_(queue),
      bus_(bus),
      active_turn_(std::move(active_turn)),
      frames_per_buffer_(cfg_.buffer_frames),
      prefill_target_samples_(
          static_cast<std::size_t>(playback_cfg_.prefill_ms)
          * static_cast<std::size_t>(cfg_.sample_rate_hz) / 1000U) {}

PlaybackEngine::~PlaybackEngine() {
    stop();
}

void PlaybackEngine::force_headless(std::chrono::milliseconds tick) {
    force_headless_.store(true, std::memory_order_release);
    headless_tick_ = tick;
}

// Audio-thread entry point — fills `frames` int16 mono samples into
// `out`, drawing from the queue. Underruns zero the tail and bump the
// counter; consumed chunks generate PlaybackFinished postbox entries
// that the publisher thread relays onto the bus.
//
// Realtime contract: never allocates; the only synchronization is the
// brief mutex inside PlaybackQueue::dequeue_active, which contends
// only with the (single-threaded) producer.
void PlaybackEngine::render_into(std::int16_t* out, std::size_t frames) {
    std::size_t filled = 0;
    while (filled < frames) {
        if (impl_->pos >= impl_->current.samples.size()) {
            // Just consumed a chunk — post a PlaybackFinished. Skip the
            // very first iteration (current.turn == kNoTurn).
            if (impl_->current.turn != event::kNoTurn
                && !impl_->current.samples.empty()) {
                (void)impl_->postbox.push(event::PlaybackFinished{
                    .turn = impl_->current.turn,
                    .seq  = impl_->current.seq,
                });
                chunks_played_.fetch_add(1, std::memory_order_relaxed);
                // Mark the chunk consumed so a subsequent underrun
                // doesn't re-publish PlaybackFinished or re-count it.
                impl_->current = AudioChunk{};
                impl_->pos = 0;
            }
            const auto active = active_turn_ ? active_turn_() : event::kNoTurn;

            // Per-turn prefill gate. When active_turn changes we
            // require `prefill_target_samples_` of audio to be queued
            // before draining starts — absorbs the streaming TTS
            // chunk-arrival jitter. While we're waiting we write
            // silence but bump prefill_silence_frames_ instead of
            // underruns_, so /metrics distinguishes intentional
            // pre-buffer fill from real starvation.
            if (prefill_target_samples_ > 0 && active != event::kNoTurn
                && primed_turn_ != active) {
                if (queue_.pending_samples_for(active) >= prefill_target_samples_) {
                    primed_turn_ = active;
                } else {
                    const std::size_t rem = frames - filled;
                    std::memset(out + filled, 0, rem * sizeof(std::int16_t));
                    prefill_silence_frames_.fetch_add(
                        rem, std::memory_order_relaxed);
                    return;
                }
            }

            auto next = queue_.dequeue_active(active);
            if (!next) {
                std::memset(out + filled, 0,
                             (frames - filled) * sizeof(std::int16_t));
                underruns_.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            impl_->current = std::move(*next);
            impl_->pos = 0;
            if (impl_->current.samples.empty()) continue;
        }
        const std::size_t take = std::min(
            frames - filled,
            impl_->current.samples.size() - impl_->pos);
        std::memcpy(out + filled,
                     impl_->current.samples.data() + impl_->pos,
                     take * sizeof(std::int16_t));
        impl_->pos += take;
        filled    += take;
    }
    frames_played_.fetch_add(frames, std::memory_order_relaxed);
}

namespace {

// PortAudio trampoline. Sits in the anonymous namespace with PortAudio's
// own typedefs visible (via the portaudio.h include at the top of this
// TU), then forwards into the engine's public render_into.
int pa_trampoline(const void* /*input*/, void* output,
                   unsigned long frame_count,
                   const PaStreamCallbackTimeInfo* /*time*/,
                   PaStreamCallbackFlags /*flags*/,
                   void* userdata) {
    auto* self = static_cast<PlaybackEngine*>(userdata);
    self->render_into(static_cast<std::int16_t*>(output),
                       static_cast<std::size_t>(frame_count));
    return paContinue;
}

} // namespace

namespace {

PaDeviceIndex pick_device(const std::string& selector) {
    if (selector.empty() || selector == "default") {
        return Pa_GetDefaultOutputDevice();
    }
    const PaDeviceIndex n = Pa_GetDeviceCount();
    for (PaDeviceIndex i = 0; i < n; ++i) {
        const auto* info = Pa_GetDeviceInfo(i);
        if (!info || info->maxOutputChannels <= 0) continue;
        std::string name = info->name ? info->name : "";
        // Case-insensitive substring match for friendly names.
        auto lower = [](std::string s) {
            for (auto& c : s) c = static_cast<char>(std::tolower(c));
            return s;
        };
        if (lower(name).find(lower(selector)) != std::string::npos) {
            return i;
        }
    }
    return paNoDevice;
}

} // namespace

bool PlaybackEngine::start() {
    if (running_.load(std::memory_order_acquire)) return true;

    // Spin up the publisher first so any early PlaybackFinished events
    // posted by the headless ticker / audio cb get delivered.
    impl_->publisher_stop.store(false, std::memory_order_release);
    impl_->publisher = std::thread([this] {
        // Drain the postbox; pop() blocks until an item arrives or the
        // queue closes during shutdown.
        while (!impl_->publisher_stop.load(std::memory_order_acquire)) {
            auto ev = impl_->postbox.pop();
            if (!ev) break;
            bus_.publish(*ev);
        }
    });

    const bool wants_headless = force_headless_.load(std::memory_order_acquire)
                                  || cfg_.output_device == "none";

    if (!wants_headless) {
        // Try to open a real PortAudio device.
        if (!impl_->pa_initialized) {
            const PaError err = Pa_Initialize();
            if (err != paNoError) {
                log::info("playback",
                    fmt::format("PortAudio init failed ({}) — using headless mode",
                                Pa_GetErrorText(err)));
            } else {
                impl_->pa_initialized = true;
            }
        }
        if (impl_->pa_initialized) {
            const auto device = pick_device(cfg_.output_device);
            if (device == paNoDevice) {
                log::info("playback",
                    fmt::format("no output device matches '{}' — headless mode",
                                cfg_.output_device));
            } else {
                PaStreamParameters out_params{};
                out_params.device = device;
                out_params.channelCount = 1;
                out_params.sampleFormat = paInt16;
                out_params.suggestedLatency =
                    Pa_GetDeviceInfo(device)->defaultLowOutputLatency;
                out_params.hostApiSpecificStreamInfo = nullptr;

                const PaError oerr = Pa_OpenStream(
                    &impl_->stream, /*input=*/nullptr, &out_params,
                    static_cast<double>(cfg_.sample_rate_hz),
                    static_cast<unsigned long>(cfg_.buffer_frames),
                    paClipOff, &pa_trampoline, this);
                if (oerr != paNoError) {
                    log::info("playback",
                        fmt::format("Pa_OpenStream failed ({}) — headless mode",
                                    Pa_GetErrorText(oerr)));
                } else {
                    const PaError serr = Pa_StartStream(impl_->stream);
                    if (serr != paNoError) {
                        log::info("playback",
                            fmt::format("Pa_StartStream failed ({}) — headless mode",
                                        Pa_GetErrorText(serr)));
                        Pa_CloseStream(impl_->stream);
                        impl_->stream = nullptr;
                    } else {
                        log::info("playback", fmt::format(
                            "PortAudio stream open: device={} rate={}Hz buf={}",
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

    // Headless fallback (or chosen explicitly).
    headless_.store(true, std::memory_order_release);
    running_.store(true, std::memory_order_release);
    impl_->headless_thread = std::thread([this] {
        std::vector<std::int16_t> scratch(frames_per_buffer_);
        while (running_.load(std::memory_order_acquire)) {
            render_into(scratch.data(), scratch.size());
            std::unique_lock lk(impl_->headless_mu);
            impl_->headless_cv.wait_for(lk, headless_tick_, [this]{
                return !running_.load(std::memory_order_acquire);
            });
        }
    });
    log::info("playback", fmt::format(
        "headless mode running (tick={} ms, buf={} frames)",
        headless_tick_.count(), frames_per_buffer_));
    return true;
}

void PlaybackEngine::stop() {
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
    {
        // Wake the headless ticker out of its cv wait_for so the
        // join() below doesn't sit on the full tick interval.
        std::lock_guard lk(impl_->headless_mu);
    }
    impl_->headless_cv.notify_all();
    if (impl_->headless_thread.joinable()) impl_->headless_thread.join();

    impl_->publisher_stop.store(true, std::memory_order_release);
    impl_->postbox.close();
    if (impl_->publisher.joinable()) impl_->publisher.join();
}

} // namespace acva::playback
