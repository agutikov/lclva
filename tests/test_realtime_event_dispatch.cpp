// Unit tests for the M5 OpenAI-Realtime event dispatcher
// (src/stt/realtime_event_dispatch.cpp).
//
// Inputs are inner JSON payloads — what the EnvelopeReassembler emits
// after unwrapping Speaches' partial_message / full_message wrapper.
// No network here.

#include "stt/realtime_event_dispatch.hpp"

#include <doctest/doctest.h>

#include <string>
#include <vector>

using acva::stt::realtime::dispatch;
using acva::stt::realtime::EventCallbacks;

namespace {

struct Capture {
    struct Partial { std::string item_id; std::string delta; };
    struct Final   { std::string item_id; std::string transcript; std::string language; };

    std::vector<Partial>     partials;
    std::vector<Final>       finals;
    std::vector<std::string> committed;
    std::vector<std::string> errors;
    int                      session_updated = 0;

    EventCallbacks bind() {
        EventCallbacks cb;
        cb.on_partial = [this](std::string id, std::string d) {
            partials.push_back({std::move(id), std::move(d)});
        };
        cb.on_final = [this](std::string id, std::string t, std::string lang) {
            finals.push_back({std::move(id), std::move(t), std::move(lang)});
        };
        cb.on_committed = [this](std::string id) {
            committed.push_back(std::move(id));
        };
        cb.on_server_error = [this](std::string m) { errors.push_back(std::move(m)); };
        cb.on_session_updated = [this] { ++session_updated; };
        return cb;
    }
};

} // namespace

TEST_CASE("dispatch: delta event fires on_partial") {
    Capture c;
    auto cb = c.bind();
    dispatch(R"({"type":"conversation.item.input_audio_transcription.delta",
                 "item_id":"item_1","delta":"hello"})", cb);
    REQUIRE(c.partials.size() == 1);
    CHECK(c.partials[0].item_id == "item_1");
    CHECK(c.partials[0].delta   == "hello");
    CHECK(c.finals.empty());
}

TEST_CASE("dispatch: completed event fires on_final and propagates language") {
    Capture c;
    auto cb = c.bind();
    dispatch(R"({"type":"conversation.item.input_audio_transcription.completed",
                 "item_id":"item_2","transcript":"Hello world.","language":"en"})", cb);
    REQUIRE(c.finals.size() == 1);
    CHECK(c.finals[0].item_id    == "item_2");
    CHECK(c.finals[0].transcript == "Hello world.");
    CHECK(c.finals[0].language   == "en");
}

TEST_CASE("dispatch: completed event with no language defaults to empty string") {
    Capture c;
    auto cb = c.bind();
    dispatch(R"({"type":"conversation.item.input_audio_transcription.completed",
                 "item_id":"item_3","transcript":"Bonjour."})", cb);
    REQUIRE(c.finals.size() == 1);
    CHECK(c.finals[0].language == "");
}

TEST_CASE("dispatch: input_audio_buffer.committed fires on_committed") {
    Capture c;
    auto cb = c.bind();
    dispatch(R"({"type":"input_audio_buffer.committed",
                 "item_id":"buf_1","previous_item_id":null})", cb);
    REQUIRE(c.committed.size() == 1);
    CHECK(c.committed[0] == "buf_1");
}

TEST_CASE("dispatch: session.updated fires lifecycle callback once") {
    Capture c;
    auto cb = c.bind();
    dispatch(R"({"type":"session.updated","event_id":"evt_x","session":{}})", cb);
    CHECK(c.session_updated == 1);
}

TEST_CASE("dispatch: error event surfaces the message") {
    Capture c;
    auto cb = c.bind();
    dispatch(R"({"type":"error","event_id":"e","error":{"type":"invalid_request_error",
                 "message":"the buffer is empty"}})", cb);
    REQUIRE(c.errors.size() == 1);
    CHECK(c.errors[0] == "the buffer is empty");
}

TEST_CASE("dispatch: error event without a message uses placeholder") {
    Capture c;
    auto cb = c.bind();
    dispatch(R"({"type":"error","error":{"type":"x"}})", cb);
    REQUIRE(c.errors.size() == 1);
    CHECK(c.errors[0] == "<no message>");
}

TEST_CASE("dispatch: ignored event types do not fire any callback") {
    Capture c;
    auto cb = c.bind();
    dispatch(R"({"type":"session.created","event_id":"e","session":{}})", cb);
    dispatch(R"({"type":"input_audio_buffer.speech_started"})", cb);
    dispatch(R"({"type":"input_audio_buffer.speech_stopped"})", cb);
    dispatch(R"({"type":"conversation.item.created"})", cb);
    dispatch(R"({"type":"response.done"})", cb);
    CHECK(c.partials.empty());
    CHECK(c.finals.empty());
    CHECK(c.committed.empty());
    CHECK(c.errors.empty());
    CHECK(c.session_updated == 0);
}

TEST_CASE("dispatch: malformed JSON drops silently") {
    Capture c;
    auto cb = c.bind();
    dispatch("not json", cb);
    dispatch("{", cb);
    dispatch("", cb);
    CHECK(c.partials.empty());
    CHECK(c.finals.empty());
}

TEST_CASE("dispatch: callbacks are optional — null entries skipped") {
    EventCallbacks cb; // all callbacks default-constructed (empty)
    // None of these should crash.
    dispatch(R"({"type":"conversation.item.input_audio_transcription.delta",
                 "item_id":"i","delta":"d"})", cb);
    dispatch(R"({"type":"conversation.item.input_audio_transcription.completed",
                 "item_id":"i","transcript":"t"})", cb);
    dispatch(R"({"type":"error","error":{"message":"m"}})", cb);
    dispatch(R"({"type":"session.updated"})", cb);
}

TEST_CASE("dispatch: events with empty item_id are dropped") {
    // Defensive: an item_id-less transcript event would be useless to
    // a per-utterance dispatch and is not part of the protocol.
    Capture c;
    auto cb = c.bind();
    dispatch(R"({"type":"conversation.item.input_audio_transcription.delta",
                 "item_id":"","delta":"x"})", cb);
    dispatch(R"({"type":"conversation.item.input_audio_transcription.completed",
                 "item_id":"","transcript":"x"})", cb);
    CHECK(c.partials.empty());
    CHECK(c.finals.empty());
}
