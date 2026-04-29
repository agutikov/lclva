#include "event/event.hpp"

namespace lclva::event {

const char* event_name(const Event& e) noexcept {
    return std::visit([]<class T>(const T&) -> const char* {
        if constexpr (std::is_same_v<T, SpeechStarted>)     return "speech_started";
        if constexpr (std::is_same_v<T, SpeechEnded>)       return "speech_ended";
        if constexpr (std::is_same_v<T, PartialTranscript>) return "partial_transcript";
        if constexpr (std::is_same_v<T, FinalTranscript>)   return "final_transcript";
        if constexpr (std::is_same_v<T, LlmStarted>)        return "llm_started";
        if constexpr (std::is_same_v<T, LlmToken>)          return "llm_token";
        if constexpr (std::is_same_v<T, LlmSentence>)       return "llm_sentence";
        if constexpr (std::is_same_v<T, LlmFinished>)       return "llm_finished";
        if constexpr (std::is_same_v<T, TtsStarted>)        return "tts_started";
        if constexpr (std::is_same_v<T, TtsAudioChunk>)     return "tts_audio_chunk";
        if constexpr (std::is_same_v<T, TtsFinished>)       return "tts_finished";
        if constexpr (std::is_same_v<T, PlaybackStarted>)   return "playback_started";
        if constexpr (std::is_same_v<T, PlaybackFinished>)  return "playback_finished";
        if constexpr (std::is_same_v<T, UserInterrupted>)   return "user_interrupted";
        if constexpr (std::is_same_v<T, CancelGeneration>)  return "cancel_generation";
        if constexpr (std::is_same_v<T, Pause>)             return "pause";
        if constexpr (std::is_same_v<T, Resume>)            return "resume";
        if constexpr (std::is_same_v<T, ErrorEvent>)        return "error";
        if constexpr (std::is_same_v<T, HealthChanged>)     return "health_changed";
        return "unknown";
    }, e);
}

TurnId event_turn(const Event& e) noexcept {
    return std::visit([]<class T>(const T& v) -> TurnId {
        if constexpr (requires { v.turn; }) {
            return v.turn;
        } else {
            return kNoTurn;
        }
    }, e);
}

} // namespace lclva::event
