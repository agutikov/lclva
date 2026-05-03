#include "orchestrator/event_tracer.hpp"

#include "event/event.hpp"
#include "log/log.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>

namespace acva::orchestrator {

namespace {

// Per-utterance counters that accumulate between LlmStarted and
// LlmFinished / between TtsStarted and TtsFinished. Held in a
// shared_ptr so the bus closure can outlive the install_*() return.
struct StreamTallies {
    std::size_t llm_tokens = 0;
    std::size_t tts_chunks = 0;
    std::size_t tts_bytes  = 0;
};

std::string truncate(std::string_view s, std::size_t max = 200) {
    if (s.size() <= max) return std::string(s);
    std::string out(s.substr(0, max));
    out += "…";
    return out;
}

} // namespace

event::SubscriptionHandle
install_event_tracer(event::EventBus& bus, const config::LoggingConfig& cfg) {
    if (!cfg.trace_events) return {};

    auto tallies = std::make_shared<StreamTallies>();

    event::SubscribeOptions opts;
    opts.name           = "trace.events";
    opts.queue_capacity = 1024;
    opts.policy         = event::OverflowPolicy::DropOldest;

    auto handle = bus.subscribe_all(opts,
        [tallies](const event::Event& e) {
            std::visit([&]<class T>(const T& ev) {
                if constexpr (std::is_same_v<T, event::SpeechStarted>) {
                    log::event("trace", "speech_started", ev.turn, {});
                } else if constexpr (std::is_same_v<T, event::SpeechEnded>) {
                    log::event("trace", "speech_ended", ev.turn, {});
                } else if constexpr (std::is_same_v<T, event::PartialTranscript>) {
                    log::event("trace", "partial_transcript", ev.turn, {
                        {"seq",  std::to_string(ev.seq)},
                        {"lang", ev.lang},
                        {"text", truncate(ev.text)},
                    });
                } else if constexpr (std::is_same_v<T, event::FinalTranscript>) {
                    log::event("trace", "final_transcript", ev.turn, {
                        {"lang",                  ev.lang},
                        {"audio_duration_ms",     std::to_string(ev.audio_duration.count())},
                        {"processing_duration_ms", std::to_string(ev.processing_duration.count())},
                        {"text",                  ev.text},
                    });
                } else if constexpr (std::is_same_v<T, event::LlmStarted>) {
                    tallies->llm_tokens = 0;
                    log::event("trace", "llm_started", ev.turn, {});
                } else if constexpr (std::is_same_v<T, event::LlmToken>) {
                    ++tallies->llm_tokens;
                } else if constexpr (std::is_same_v<T, event::LlmSentence>) {
                    log::event("trace", "llm_sentence", ev.turn, {
                        {"seq",  std::to_string(ev.seq)},
                        {"lang", ev.lang},
                        {"text", ev.text},
                    });
                } else if constexpr (std::is_same_v<T, event::LlmFinished>) {
                    log::event("trace", "llm_finished", ev.turn, {
                        {"tokens",    std::to_string(tallies->llm_tokens)},
                        {"cancelled", ev.cancelled ? "true" : "false"},
                    });
                } else if constexpr (std::is_same_v<T, event::TtsStarted>) {
                    log::event("trace", "tts_started", ev.turn, {
                        {"seq", std::to_string(ev.seq)},
                    });
                } else if constexpr (std::is_same_v<T, event::TtsAudioChunk>) {
                    tallies->tts_chunks += 1;
                    tallies->tts_bytes  += ev.bytes;
                } else if constexpr (std::is_same_v<T, event::TtsFinished>) {
                    log::event("trace", "tts_finished", ev.turn, {
                        {"seq",    std::to_string(ev.seq)},
                        {"chunks", std::to_string(tallies->tts_chunks)},
                        {"bytes",  std::to_string(tallies->tts_bytes)},
                    });
                    tallies->tts_chunks = 0;
                    tallies->tts_bytes  = 0;
                } else if constexpr (std::is_same_v<T, event::PlaybackStarted>) {
                    log::event("trace", "playback_started", ev.turn, {
                        {"seq", std::to_string(ev.seq)},
                    });
                } else if constexpr (std::is_same_v<T, event::PlaybackFinished>) {
                    log::event("trace", "playback_finished", ev.turn, {
                        {"seq", std::to_string(ev.seq)},
                    });
                } else if constexpr (std::is_same_v<T, event::UserInterrupted>) {
                    log::event("trace", "user_interrupted", ev.turn, {});
                } else if constexpr (std::is_same_v<T, event::CancelGeneration>) {
                    log::event("trace", "cancel_generation", ev.turn, {});
                }
                // UtteranceReady / Pause / Resume / ErrorEvent /
                // HealthChanged are handled elsewhere or have low
                // signal value here.
            }, e);
        });

    log::info("main", "event tracer enabled (cfg.logging.trace_events)");
    return handle;
}

} // namespace acva::orchestrator
