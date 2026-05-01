#pragma once

#include <chrono>
#include <cstdint>

namespace acva::audio {

// VAD-driven utterance endpoint detector.
//
// Drives a four-state machine off the per-frame Silero probability:
//
//   Quiet      — below onset_threshold; no candidate utterance pending.
//   Onset      — probability has crossed onset_threshold; we wait
//                until min_speech_ms of consistent speech has
//                accumulated before promoting to Speaking. This
//                filters momentary noise spikes.
//   Speaking   — utterance underway. Emits SpeechStarted on the
//                Onset → Speaking transition, with the pre-padded
//                buffer ready in the rolling pre-buffer.
//   Endpoint   — probability has dropped below offset_threshold; we
//                wait `hangover_ms` of continuous low-probability
//                frames before emitting SpeechEnded. Going back above
//                onset_threshold within the hangover keeps us in
//                Speaking.
//
// The Endpointer itself doesn't own the audio buffer — it reports
// outcomes to the caller, who appends incoming 16 kHz frames to a
// matching UtteranceBuffer (Step 6) and consumes pre/post padding from
// it.
struct EndpointerConfig {
    float onset_threshold  = 0.5F;
    float offset_threshold = 0.35F;
    std::chrono::milliseconds min_speech_ms{200};
    std::chrono::milliseconds hangover_ms{600};
    std::chrono::milliseconds pre_padding_ms{300};
    std::chrono::milliseconds post_padding_ms{100};
};

enum class EndpointerState : std::uint8_t {
    Quiet,
    Onset,
    Speaking,
    Endpoint,
};

[[nodiscard]] const char* to_string(EndpointerState s) noexcept;

class Endpointer {
public:
    enum class FrameOutcome : std::uint8_t {
        None,            // no transition
        SpeechStarted,   // crossed onset → Speaking; consume pre-buffer
        FalseStart,      // Onset never matured into Speaking
        SpeechEnded,     // hangover elapsed; finalize the utterance
    };

    explicit Endpointer(EndpointerConfig cfg = {}, std::uint32_t sample_rate = 16000);

    // Feed one frame's VAD probability + duration. `frame_duration` is
    // typically 32 ms (Silero window) or 10 ms (post-resample frame).
    // Returns the transition observed on this frame, or None.
    FrameOutcome on_frame(float vad_probability,
                            std::chrono::milliseconds frame_duration,
                            std::chrono::steady_clock::time_point frame_time);

    // Force the current Speaking utterance to end (e.g., assistant
    // about to play TTS — barge-in handling cuts the capture). Returns
    // SpeechEnded if there was an utterance in progress, None otherwise.
    FrameOutcome force_endpoint(std::chrono::steady_clock::time_point now);

    [[nodiscard]] EndpointerState state() const noexcept { return state_; }
    [[nodiscard]] const EndpointerConfig& config() const noexcept { return cfg_; }

    // Rolling timestamps useful to UtteranceBuffer (and tests).
    [[nodiscard]] std::chrono::steady_clock::time_point started_at() const noexcept { return started_at_; }
    [[nodiscard]] std::chrono::steady_clock::time_point ended_at()   const noexcept { return ended_at_; }

    // Accumulated speech / quiet durations during the current state.
    [[nodiscard]] std::chrono::milliseconds elapsed_speech() const noexcept { return speech_dur_; }
    [[nodiscard]] std::chrono::milliseconds elapsed_quiet()  const noexcept { return quiet_dur_; }

    // Reset to Quiet (e.g. after the consumer accepted the utterance).
    void reset();

private:
    EndpointerConfig cfg_;
    std::uint32_t    sample_rate_ = 16000;

    EndpointerState  state_ = EndpointerState::Quiet;
    std::chrono::milliseconds speech_dur_{0};
    std::chrono::milliseconds quiet_dur_{0};
    std::chrono::steady_clock::time_point started_at_{};
    std::chrono::steady_clock::time_point ended_at_{};
};

} // namespace acva::audio
