#include "audio/endpointer.hpp"

namespace acva::audio {

const char* to_string(EndpointerState s) noexcept {
    switch (s) {
        case EndpointerState::Quiet:    return "quiet";
        case EndpointerState::Onset:    return "onset";
        case EndpointerState::Speaking: return "speaking";
        case EndpointerState::Endpoint: return "endpoint";
    }
    return "unknown";
}

Endpointer::Endpointer(EndpointerConfig cfg, std::uint32_t sample_rate)
    : cfg_(cfg), sample_rate_(sample_rate) {}

void Endpointer::reset() {
    state_ = EndpointerState::Quiet;
    speech_dur_ = std::chrono::milliseconds{0};
    quiet_dur_  = std::chrono::milliseconds{0};
    started_at_ = {};
    ended_at_   = {};
}

Endpointer::FrameOutcome
Endpointer::on_frame(float vad_probability,
                     std::chrono::milliseconds frame_duration,
                     std::chrono::steady_clock::time_point frame_time) {
    const bool above_onset  = vad_probability >= cfg_.onset_threshold;
    const bool below_offset = vad_probability <  cfg_.offset_threshold;

    switch (state_) {
        case EndpointerState::Quiet: {
            if (above_onset) {
                state_      = EndpointerState::Onset;
                speech_dur_ = frame_duration;
                quiet_dur_  = std::chrono::milliseconds{0};
                started_at_ = frame_time;
            }
            return FrameOutcome::None;
        }

        case EndpointerState::Onset: {
            if (above_onset) {
                speech_dur_ += frame_duration;
                if (speech_dur_ >= cfg_.min_speech_ms) {
                    state_     = EndpointerState::Speaking;
                    quiet_dur_ = std::chrono::milliseconds{0};
                    return FrameOutcome::SpeechStarted;
                }
                return FrameOutcome::None;
            }
            if (below_offset) {
                // The candidate didn't mature — drop back to Quiet
                // and report it so the caller can bump the
                // false-start counter.
                state_      = EndpointerState::Quiet;
                speech_dur_ = std::chrono::milliseconds{0};
                quiet_dur_  = std::chrono::milliseconds{0};
                started_at_ = {};
                return FrameOutcome::FalseStart;
            }
            // In the [offset, onset) hysteresis band: keep waiting.
            return FrameOutcome::None;
        }

        case EndpointerState::Speaking: {
            if (above_onset) {
                speech_dur_ += frame_duration;
                quiet_dur_  = std::chrono::milliseconds{0};
                return FrameOutcome::None;
            }
            if (below_offset) {
                // Hangover starts now; mark the tentative endpoint
                // timestamp. We stay in Speaking visually but quiet
                // accumulation is the gate. Crossing back above
                // onset before hangover_ms cancels the endpoint.
                state_     = EndpointerState::Endpoint;
                quiet_dur_ = frame_duration;
                ended_at_  = frame_time;
                return FrameOutcome::None;
            }
            // Hysteresis band — treat as still speaking.
            speech_dur_ += frame_duration;
            return FrameOutcome::None;
        }

        case EndpointerState::Endpoint: {
            if (above_onset) {
                state_     = EndpointerState::Speaking;
                speech_dur_ += frame_duration;
                quiet_dur_  = std::chrono::milliseconds{0};
                ended_at_   = {};
                return FrameOutcome::None;
            }
            if (below_offset) {
                quiet_dur_ += frame_duration;
                if (quiet_dur_ >= cfg_.hangover_ms) {
                    // Lock in the endpoint at the most recent quiet
                    // frame's timestamp so post-padding lands at the
                    // utterance tail, not the hangover tail.
                    state_ = EndpointerState::Quiet;
                    return FrameOutcome::SpeechEnded;
                }
                return FrameOutcome::None;
            }
            // Hysteresis band during hangover — neither extending
            // speech nor closing the gap. Keep accumulating quiet so
            // the hangover still elapses on continued indecision.
            quiet_dur_ += frame_duration;
            if (quiet_dur_ >= cfg_.hangover_ms) {
                state_ = EndpointerState::Quiet;
                return FrameOutcome::SpeechEnded;
            }
            return FrameOutcome::None;
        }
    }
    return FrameOutcome::None;
}

Endpointer::FrameOutcome
Endpointer::force_endpoint(std::chrono::steady_clock::time_point now) {
    if (state_ == EndpointerState::Speaking
        || state_ == EndpointerState::Endpoint) {
        ended_at_ = now;
        state_    = EndpointerState::Quiet;
        return FrameOutcome::SpeechEnded;
    }
    if (state_ == EndpointerState::Onset) {
        state_ = EndpointerState::Quiet;
        return FrameOutcome::FalseStart;
    }
    return FrameOutcome::None;
}

} // namespace acva::audio
