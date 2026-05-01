#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace acva::stt::realtime {

// Speaches wraps every event it sends on the WebRTC `oai-events` data
// channel in one of two envelopes (see project memory note
// `project_m5_realtime_spike.md`):
//
//   {"id":"event_…","type":"full_message","data":"<base64>"}
//
//   {"id":"event_…","type":"partial_message","data":"<base64>",
//    "fragment_index":N,"total_fragments":M}
//
// The inner payload (after base64-decode) is the actual OpenAI
// Realtime API event JSON. EnvelopeReassembler accumulates fragments
// keyed by `id` and emits the inner JSON exactly once per event.
//
// Outgoing direction (client → server) is **not** wrapped — Speaches'
// `message_handler` validates raw OpenAI Realtime client events. So
// this header is server→client only.
//
// Single-threaded: the data-channel `onMessage` callback runs on
// libdatachannel's internal thread, which delivers messages serially
// per channel. No locking needed inside the reassembler itself; the
// owning RealtimeSttClient hands inner-JSON strings off to its event
// dispatcher under its own synchronization.

class EnvelopeReassembler {
public:
    // Feed one raw data-channel message (the JSON envelope verbatim).
    // Returns the inner OpenAI Realtime event JSON when a complete
    // message has been assembled (full_message: same call;
    // partial_message: when the last fragment arrives). Returns
    // std::nullopt for partials still in flight, malformed input, or
    // unexpected envelope types — malformed input is dropped silently
    // (Speaches' protocol is server-controlled; an invalid envelope
    // is a server bug we'd surface via metrics, not a recoverable
    // client error).
    std::optional<std::string> feed(std::string_view raw);

    // Number of partial events with at least one fragment received but
    // not yet completed. Useful as a metric / leak detector.
    [[nodiscard]] std::size_t pending_count() const noexcept {
        return pending_.size();
    }

private:
    struct Pending {
        std::vector<std::string> fragments;  // indexed by fragment_index
        std::size_t              received = 0;
    };
    std::unordered_map<std::string, Pending> pending_;
};

// Base64 decoder for the `data` field. Standard alphabet, padding
// expected. Returns std::nullopt on malformed input.
std::optional<std::string> base64_decode(std::string_view input);

} // namespace acva::stt::realtime
