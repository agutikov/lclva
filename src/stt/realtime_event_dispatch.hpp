#pragma once

#include <functional>
#include <string>
#include <string_view>

namespace acva::stt::realtime {

// Callbacks invoked by `dispatch()` for the OpenAI-Realtime events
// the M5 STT client cares about. Each callback is optional — a null
// std::function is silently skipped. All event types not listed here
// (session.created, session.updated, input_audio_buffer.speech_*,
// response.*, conversation.item.created, etc.) are ignored.
//
// Threading: `dispatch()` is synchronous on the caller's thread and
// invokes callbacks inline. The owning RealtimeSttClient calls it
// from libdatachannel's data-channel thread; callback bodies must be
// careful with their own synchronization.
struct EventCallbacks {
    // `conversation.item.input_audio_transcription.delta` — emitted
    // repeatedly as the server transcribes the in-flight buffer. The
    // delta is the new text appended since the last delta on the
    // same item; assemblers should concatenate.
    std::function<void(std::string item_id, std::string delta)> on_partial;

    // `conversation.item.input_audio_transcription.completed` — emitted
    // once after `input_audio_buffer.commit` (or when server VAD
    // closes a turn). `transcript` is the full final transcription.
    // Includes `language` if Speaches detected one (currently best-
    // effort — Speaches' default `input_audio_transcription.model =
    // distil-small.en` does not detect language, but
    // large-v3-turbo-ct2 does).
    std::function<void(std::string item_id,
                       std::string transcript,
                       std::string language)> on_final;

    // `input_audio_buffer.committed` — server confirms our commit and
    // returns the item id we'll see on subsequent transcription events.
    std::function<void(std::string item_id)> on_committed;

    // `error` — server-side error event. Surface for logging /
    // metrics; do not propagate to the user.
    std::function<void(std::string message)> on_server_error;

    // `session.updated` — used by start() to detect Ready state.
    // Separately exposed so the lifecycle path doesn't entangle with
    // per-utterance handlers.
    std::function<void()> on_session_updated;
};

// Parse one inner OpenAI-Realtime event JSON (already unwrapped from
// Speaches' fragmentation envelope) and dispatch to the matching
// callback. Unknown types are dropped silently.
void dispatch(std::string_view inner_json, const EventCallbacks& cb);

} // namespace acva::stt::realtime
