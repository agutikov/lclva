#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>

namespace lclva::event {

// TurnId is also defined in dialogue/turn.hpp; kept as a typedef here so
// event types do not transitively pull dialogue headers. Single source of
// truth: dialogue::TurnId. We use the same underlying type.
using TurnId = std::uint64_t;
inline constexpr TurnId kNoTurn = 0;

using SequenceNo = std::uint32_t;

// ----- Speech / VAD events -----

struct SpeechStarted {
    TurnId turn = kNoTurn;
    std::chrono::steady_clock::time_point ts{};
};

struct SpeechEnded {
    TurnId turn = kNoTurn;
    std::chrono::steady_clock::time_point ts{};
};

// ----- STT events -----

struct PartialTranscript {
    TurnId turn = kNoTurn;
    std::string text;
    std::string lang;          // BCP-47 (en, ru, de, ...)
    std::size_t stable_prefix_len = 0;
    SequenceNo seq = 0;
    float confidence = 0.0f;
};

struct FinalTranscript {
    TurnId turn = kNoTurn;
    std::string text;
    std::string lang;
    float confidence = 0.0f;
    std::chrono::milliseconds audio_duration{0};
    std::chrono::milliseconds processing_duration{0};
};

// ----- LLM events -----

struct LlmStarted {
    TurnId turn = kNoTurn;
};

struct LlmToken {
    TurnId turn = kNoTurn;
    std::string token;
};

// Emitted by Dialogue when SentenceSplitter completes a sentence.
struct LlmSentence {
    TurnId turn = kNoTurn;
    SequenceNo seq = 0;
    std::string text;
    std::string lang;
};

struct LlmFinished {
    TurnId turn = kNoTurn;
    bool cancelled = false;
    std::size_t tokens_generated = 0;
};

// ----- TTS events -----

struct TtsStarted {
    TurnId turn = kNoTurn;
    SequenceNo seq = 0;
};

struct TtsAudioChunk {
    TurnId turn = kNoTurn;
    SequenceNo seq = 0;
    std::size_t bytes = 0;
};

struct TtsFinished {
    TurnId turn = kNoTurn;
    SequenceNo seq = 0;
};

// ----- Playback events -----

struct PlaybackStarted {
    TurnId turn = kNoTurn;
    SequenceNo seq = 0;
};

struct PlaybackFinished {
    TurnId turn = kNoTurn;
    SequenceNo seq = 0;
};

// ----- Control events -----

struct UserInterrupted {
    TurnId turn = kNoTurn;          // turn that was interrupted
    std::chrono::steady_clock::time_point ts{};
};

struct CancelGeneration {
    TurnId turn = kNoTurn;
};

struct Pause {};
struct Resume {};

// ----- System events -----

struct ErrorEvent {
    std::string component;
    std::string message;
};

enum class HealthState {
    Unknown,
    Healthy,
    Degraded,
    Unhealthy,
};

struct HealthChanged {
    std::string service;
    HealthState state = HealthState::Unknown;
    std::string detail;
};

// The Event variant covers everything that flows on the control bus.
// Audio frames have a separate, lock-free SPSC ring (M4) — they do NOT travel
// through the event bus.
using Event = std::variant<
    SpeechStarted,
    SpeechEnded,
    PartialTranscript,
    FinalTranscript,
    LlmStarted,
    LlmToken,
    LlmSentence,
    LlmFinished,
    TtsStarted,
    TtsAudioChunk,
    TtsFinished,
    PlaybackStarted,
    PlaybackFinished,
    UserInterrupted,
    CancelGeneration,
    Pause,
    Resume,
    ErrorEvent,
    HealthChanged
>;

// Short, stable name for an event type. Used in metric labels and logs.
[[nodiscard]] const char* event_name(const Event& e) noexcept;

// Extract the turn id if the event has one. Returns kNoTurn for events
// without a turn (Pause/Resume/HealthChanged/ErrorEvent).
[[nodiscard]] TurnId event_turn(const Event& e) noexcept;

} // namespace lclva::event
