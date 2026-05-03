#pragma once

#include "config/config.hpp"
#include "event/bus.hpp"

namespace acva::orchestrator {

// Install the bus subscriber that emits one structured `trace` log
// line per interesting event (SpeechStarted/Ended, FinalTranscript,
// LlmStarted/Sentence/Finished, TtsStarted/Finished, PlaybackStarted/
// Finished, UserInterrupted, CancelGeneration). Per-token / per-chunk
// events are aggregated into the next "finished" event so the log
// stays readable. Returns the SubscriptionHandle which the caller
// must keep alive for the run.
//
// No-op (returns a default-constructed handle) when
// cfg.logging.trace_events is false.
[[nodiscard]] event::SubscriptionHandle
install_event_tracer(event::EventBus& bus, const config::LoggingConfig& cfg);

} // namespace acva::orchestrator
