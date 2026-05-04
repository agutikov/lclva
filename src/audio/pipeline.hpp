#pragma once

#include "audio/apm.hpp"
#include "audio/capture.hpp"
#include "audio/clock.hpp"
#include "audio/endpointer.hpp"
#include "audio/resampler.hpp"
#include "audio/utterance.hpp"
#include "audio/vad.hpp"
#include "event/bus.hpp"

#include <string>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <thread>

namespace acva::audio {

class LoopbackSink;

// Owns the consumer half of the M4 capture path: drains the SPSC ring
// (filled by CaptureEngine on PortAudio's callback thread), resamples
// 48 → 16 kHz, runs Silero VAD + Endpointer, assembles utterances,
// and publishes SpeechStarted / SpeechEnded / UtteranceReady on the
// event bus.
//
// One dedicated worker thread, never the audio callback thread. The
// resampler + VAD allocate; that's why they live here rather than in
// CaptureEngine.
class AudioPipeline {
public:
    struct Config {
        std::uint32_t            input_sample_rate = 48000;
        std::uint32_t            output_sample_rate = 16000;
        EndpointerConfig         endpointer{};
        std::chrono::milliseconds pre_padding_ms{300};
        std::chrono::milliseconds post_padding_ms{100};
        std::size_t              max_in_flight = 3;
        std::chrono::milliseconds max_duration_ms{60000};
        // Empty disables Silero — we still emit Speech* events
        // driven by a stub probability of 0 (i.e., never). The stub
        // is useful only as a placeholder; the loopback demo runs in
        // this mode for VAD-free testing.
        std::string vad_model_path;

        // M6 — when `loopback` is non-null AND `apm.aec_enabled`, the
        // pipeline inserts an APM stage between the resampler and the
        // VAD: `SPSC ring → Resample → APM → VAD → Endpointer`.
        // Reference samples come from the loopback sink (filled by
        // PlaybackEngine on every render_into). When `loopback` is null
        // the APM stage is skipped entirely; existing M4 behaviour is
        // preserved bit-for-bit so capture-only tests don't regress.
        ApmConfig     apm{};
        LoopbackSink* loopback = nullptr;
        // M7 Bug 4 — minimum int16 RMS for an utterance slice to be
        // emitted as UtteranceReady. Slices below this are dropped
        // (logged at info level + counted) so Whisper never sees the
        // near-silence frames that produce its subtitle hallucinations.
        // 0 disables the gate. Sourced from cfg.stt.min_utterance_rms.
        std::uint32_t min_utterance_rms = 0;
    };

    AudioPipeline(Config cfg,
                   CaptureRing& ring,
                   MonotonicAudioClock& clock,
                   event::EventBus& bus);
    ~AudioPipeline();

    AudioPipeline(const AudioPipeline&)            = delete;
    AudioPipeline& operator=(const AudioPipeline&) = delete;
    AudioPipeline(AudioPipeline&&)                 = delete;
    AudioPipeline& operator=(AudioPipeline&&)      = delete;

    void start();
    void stop();

    [[nodiscard]] std::uint64_t frames_processed()  const noexcept { return frames_processed_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t utterances_total()  const noexcept { return utterances_total_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t false_starts_total() const noexcept { return false_starts_total_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t low_rms_drops_total() const noexcept { return low_rms_drops_total_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t utterance_drops()    const noexcept { return utterance_buffer_.drops(); }
    [[nodiscard]] std::size_t   in_flight()          const noexcept { return utterance_buffer_.in_flight(); }
    [[nodiscard]] bool          vad_enabled()        const noexcept { return vad_ != nullptr; }
    // M6 observability — non-null whenever the APM stage is wired in.
    // Returns nullptr in stub builds (ACVA_HAVE_WEBRTC_APM undefined),
    // when no loopback was passed, or when AEC was disabled in cfg.
    [[nodiscard]] const Apm*    apm()                const noexcept { return apm_.get(); }

    // Inspect the current endpointer state — useful for the capture demo.
    [[nodiscard]] EndpointerState endpointer_state() const noexcept { return endpointer_.state(); }

    // For tests: drain the ring N times synchronously without spawning
    // the worker thread. Returns the number of frames processed.
    std::size_t pump_for_test(std::size_t max_frames);

    // For tests: force the VAD probability for the next pump cycle,
    // bypassing the model. Useful for end-to-end pipeline tests that
    // shouldn't depend on Silero. -1.0 disables the override and falls
    // back to the real VAD path.
    void set_test_probability(float p) noexcept { test_probability_ = p; }

    // M5 — sink for the streaming STT path. Called from the pipeline
    // worker thread once per resampled chunk while between
    // SpeechStarted and SpeechEnded with the 16 kHz mono int16
    // samples. The chunk itself is also appended to the
    // UtteranceBuffer for the M4B request/response client, so both
    // STT paths can coexist (in practice main.cpp picks one). The
    // sink runs synchronously inside the worker — it must not block
    // (`RealtimeSttClient::push_audio` queues on libdatachannel's
    // SCTP buffer and returns).
    using LiveAudioSink = std::function<void(std::span<const std::int16_t>)>;
    void set_live_audio_sink(LiveAudioSink sink) { live_sink_ = std::move(sink); }

private:
    void run_loop();
    void process_frame(const AudioFrame& frame);

    Config           cfg_;
    CaptureRing&     ring_;
    MonotonicAudioClock& clock_;
    event::EventBus& bus_;

    Resampler        resampler_;
    std::unique_ptr<SileroVad> vad_;
    std::unique_ptr<Apm>       apm_;
    Endpointer       endpointer_;
    UtteranceBuffer  utterance_buffer_;

    std::thread       worker_;
    std::atomic<bool> running_{false};

    std::atomic<std::uint64_t> frames_processed_{0};
    std::atomic<std::uint64_t> utterances_total_{0};
    std::atomic<std::uint64_t> false_starts_total_{0};
    std::atomic<std::uint64_t> low_rms_drops_total_{0};

    // Cached VAD probability between Silero updates (it processes 32 ms
    // windows; we may step the endpointer at the resampled frame rate
    // ≈ 10 ms, so the same probability applies to multiple frames).
    float last_vad_p_ = 0.0F;

    // M6 — APM input buffer. soxr's variable chunk sizes (e.g.
    // 192/106/192 from a 480→160 ratio) don't line up with APM's
    // required 10-ms / 160-sample frames; we accumulate resampled
    // output here and pull complete blocks out for ProcessStream.
    // Empty when APM is not active.
    std::vector<std::int16_t> apm_carry_;

    // Test override (-1 = inactive, otherwise replaces last_vad_p_).
    float test_probability_ = -1.0F;

    // M5 streaming sink (see set_live_audio_sink). Set/queried only
    // from the worker thread + the main thread before start(); no
    // synchronization needed.
    LiveAudioSink live_sink_;
    bool          in_speech_ = false;
};

} // namespace acva::audio
