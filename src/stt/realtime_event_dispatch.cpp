#include "stt/realtime_event_dispatch.hpp"

#include <glaze/glaze.hpp>

#include <optional>
#include <string>
#include <utility>

namespace acva::stt::realtime {

namespace {

constexpr glz::opts kReadOpts{.error_on_unknown_keys = false};

struct TypeOnly {
    std::string type;
};

struct DeltaEvent {
    std::string item_id;
    std::string delta;
};

struct CompletedEvent {
    std::string                item_id;
    std::string                transcript;
    std::optional<std::string> language;
};

struct CommittedEvent {
    std::string item_id;
};

struct ErrorPayload {
    std::optional<std::string> message;
};
struct ErrorEnvelope {
    ErrorPayload error;
};

template <typename T>
bool read(T& out, std::string_view json) {
    return !glz::read<kReadOpts>(out, json);
}

} // namespace

void dispatch(std::string_view inner_json, const EventCallbacks& cb) {
    TypeOnly t;
    if (!read(t, inner_json)) return;

    if (t.type == "conversation.item.input_audio_transcription.delta") {
        if (!cb.on_partial) return;
        DeltaEvent ev;
        if (!read(ev, inner_json)) return;
        if (ev.item_id.empty() || ev.delta.empty()) return;
        cb.on_partial(std::move(ev.item_id), std::move(ev.delta));
        return;
    }

    if (t.type == "conversation.item.input_audio_transcription.completed") {
        if (!cb.on_final) return;
        CompletedEvent ev;
        if (!read(ev, inner_json)) return;
        if (ev.item_id.empty()) return;
        cb.on_final(std::move(ev.item_id),
                    std::move(ev.transcript),
                    ev.language.value_or(std::string{}));
        return;
    }

    if (t.type == "input_audio_buffer.committed") {
        if (!cb.on_committed) return;
        CommittedEvent ev;
        if (!read(ev, inner_json)) return;
        if (ev.item_id.empty()) return;
        cb.on_committed(std::move(ev.item_id));
        return;
    }

    if (t.type == "session.updated") {
        if (cb.on_session_updated) cb.on_session_updated();
        return;
    }

    if (t.type == "error") {
        if (!cb.on_server_error) return;
        ErrorEnvelope env;
        if (!read(env, inner_json)) {
            cb.on_server_error("<unparseable error event>");
            return;
        }
        cb.on_server_error(env.error.message.value_or("<no message>"));
        return;
    }

    // Unknown / uninteresting types: session.created, response.*,
    // conversation.item.created, input_audio_buffer.speech_*, etc.
    // Drop silently.
}

} // namespace acva::stt::realtime
