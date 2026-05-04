#include "playback/engine.hpp"

#include "audio/loopback.hpp"
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
void PlaybackEngine::note_barge_in(dialogue::TurnId cancelled_turn,
                                    std::chrono::steady_clock::time_point publish_ts) noexcept {
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        publish_ts.time_since_epoch()).count();
    barge_in_target_turn_.store(cancelled_turn, std::memory_order_release);
    barge_in_publish_ns_.store(ns, std::memory_order_release);
}

double PlaybackEngine::consume_pending_barge_in_latency_ms() noexcept {
    const double v = barge_in_pending_latency_ms_.exchange(-1.0, std::memory_order_acq_rel);
    return v;
}

namespace {

// Audio-thread helper: if a barge-in is armed and we just emitted a
// silent buffer for a turn that has moved past the cancelled one, stop
// the timer and stash the delta. Single-shot per arm.
inline void maybe_close_barge_in_timer(
        std::atomic<std::int64_t>&     publish_ns,
        std::atomic<dialogue::TurnId>& target_turn,
        std::atomic<double>&           pending_ms,
        dialogue::TurnId               active_turn) noexcept {
    const auto ns = publish_ns.load(std::memory_order_acquire);
    if (ns == 0) return;
    const auto target = target_turn.load(std::memory_order_acquire);
    // We treat "audio is silent" as "the active turn is not the cancelled
    // turn anymore" — once the FSM has bumped the active id, the audio
    // thread is rendering silence (or the next user-induced turn's audio).
    // Comparing against `target` rather than `event::kNoTurn` covers the
    // back-to-back-turn case where active flips straight to the new id.
    if (active_turn == target) return;
    const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    // CAS publish_ns to 0 so we record the latency exactly once even if
    // multiple silent buffers fire before the poller drains it.
    auto expected = ns;
    if (publish_ns.compare_exchange_strong(expected, 0,
                                            std::memory_order_acq_rel,
                                            std::memory_order_relaxed)) {
        const double ms = static_cast<double>(now_ns - ns) / 1.0e6;
        pending_ms.store(ms, std::memory_order_release);
    }
}

} // namespace

void PlaybackEngine::render_into(std::int16_t* out, std::size_t frames) {
    std::size_t filled = 0;
    while (filled < frames) {
        if (impl_->pos >= impl_->current.samples.size()) {
            // Just consumed a chunk. Bump the chunks-played counter
            // for any real chunk (non-empty); fire PlaybackFinished
            // ONCE per sentence (on chunks marked end_of_sentence by
            // the producer). Subsequent underruns / re-entries
            // mustn't re-fire — clear `current` to AudioChunk{} below.
            if (impl_->current.turn != event::kNoTurn) {
                if (!impl_->current.samples.empty()) {
                    chunks_played_.fetch_add(1, std::memory_order_relaxed);
                }
                if (impl_->current.end_of_sentence) {
                    (void)impl_->postbox.push(event::PlaybackFinished{
                        .turn = impl_->current.turn,
                        .seq  = impl_->current.seq,
                    });
                }
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
                    maybe_close_barge_in_timer(
                        barge_in_publish_ns_, barge_in_target_turn_,
                        barge_in_pending_latency_ms_, active);
                    return;
                }
            }

            auto next = queue_.dequeue_active(active);
            if (!next) {
                std::memset(out + filled, 0,
                             (frames - filled) * sizeof(std::int16_t));
                underruns_.fetch_add(1, std::memory_order_relaxed);
                maybe_close_barge_in_timer(
                    barge_in_publish_ns_, barge_in_target_turn_,
                    barge_in_pending_latency_ms_, active);
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

    // M6 reference-signal tap. After `out` is fully populated (with
    // either real samples or silence — both are what the device will
    // emit), copy into the loopback ring with the current wall-clock
    // instant. The APM wrapper later pulls `aligned(capture_time, …)`
    // to recover the reference for AEC.
    //
    // We use steady_clock::now() rather than PortAudio's
    // outputBufferDacTime because the conversion between PA stream-time
    // and steady_clock costs another syscall; the worst-case error is
    // one buffer length (~10 ms) which sits well under the APM's
    // built-in delay-estimate tolerance.
    if (loopback_sink_ != nullptr) {
        loopback_sink_->on_emitted(
            std::span<const std::int16_t>{out, frames},
            std::chrono::steady_clock::now());
    }
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
    // headless_tick_ == 0 → "don't spawn the ticker, just the
    // publisher". Used by the unit test that drives render_into
    // directly and otherwise loses to a tiny race where the ticker
    // gets one render_into call in before its first sleep.
    if (headless_tick_.count() > 0) {
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
    } else {
        log::info("playback", fmt::format(
            "headless mode running (manual; tick=0; buf={} frames)",
            frames_per_buffer_));
    }
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
