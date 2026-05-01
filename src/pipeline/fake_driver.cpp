#include "pipeline/fake_driver.hpp"

#include "log/log.hpp"

#include <fmt/format.h>

#include <random>
#include <utility>

namespace acva::pipeline {

namespace {

void sleep_for_or_stop(std::chrono::milliseconds dur, std::atomic<bool>& running) {
    constexpr auto step = std::chrono::milliseconds{50};
    auto remaining = dur;
    while (remaining > std::chrono::milliseconds::zero()
           && running.load(std::memory_order_acquire)) {
        const auto chunk = remaining < step ? remaining : step;
        std::this_thread::sleep_for(chunk);
        remaining -= chunk;
    }
}

} // namespace

FakeDriver::FakeDriver(event::EventBus& bus, FakeDriverOptions opts)
    : bus_(bus), opts_(std::move(opts)) {}

FakeDriver::~FakeDriver() {
    stop();
}

void FakeDriver::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return;
    }
    worker_ = std::thread([this] { run_loop(); });
}

void FakeDriver::stop() {
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) {
        return;
    }
    if (worker_.joinable()) {
        worker_.join();
    }
}

void FakeDriver::run_loop() {
    log::info("fake_driver", "started");

    // Turn ids are minted by the FSM via TurnFactory when SpeechStarted
    // arrives. The driver doesn't allocate ids — it learns them from the
    // FSM snapshot if we wanted to be precise, but for an end-to-end synth
    // pipeline we just emit kNoTurn-tagged speech events; the FSM's
    // SpeechStarted handler fills in the active turn. Subsequent events in
    // this turn must carry that id, so we read it back from the snapshot.
    //
    // To stay self-contained, the driver keeps its own counter mirroring the
    // FSM's. Both start at 1; in M0 nobody else publishes SpeechStarted so
    // they stay in sync.
    event::TurnId turn = 1;

    std::mt19937_64 rng(opts_.seed != 0
                        ? opts_.seed
                        : static_cast<std::uint64_t>(
                              std::chrono::steady_clock::now().time_since_epoch().count()));
    std::uniform_real_distribution<double> uniform{0.0, 1.0};

    while (running_.load(std::memory_order_acquire)) {
        sleep_for_or_stop(opts_.idle_between_turns, running_);
        if (!running_.load(std::memory_order_acquire)) break;

        const bool barge_in = uniform(rng) < opts_.barge_in_probability;
        if (barge_in) {
            log::info("fake_driver",
                      fmt::format("turn {} will be interrupted mid-playback", turn));
            run_one_turn_with_barge_in(turn);
        } else {
            run_one_turn(turn);
        }
        if (!running_.load(std::memory_order_acquire)) break;

        ++turn;
        turns_emitted_.fetch_add(1, std::memory_order_relaxed);
    }

    log::info("fake_driver", fmt::format("stopped after {} turns",
                                          turns_emitted_.load(std::memory_order_relaxed)));
}

void FakeDriver::run_one_turn(event::TurnId turn) {
    // SpeechStarted (skipped when M4 capture owns these events).
    if (!opts_.suppress_speech_events) {
        bus_.publish(event::SpeechStarted{
            .turn = turn,
            .ts = std::chrono::steady_clock::now(),
        });
    }

    sleep_for_or_stop(opts_.user_speech_duration, running_);
    if (!running_.load(std::memory_order_acquire)) return;

    if (!opts_.suppress_speech_events) {
        bus_.publish(event::SpeechEnded{
            .turn = turn,
            .ts = std::chrono::steady_clock::now(),
        });
    }

    sleep_for_or_stop(opts_.stt_processing, running_);
    if (!running_.load(std::memory_order_acquire)) return;

    bus_.publish(event::FinalTranscript{
        .turn = turn,
        .text = "what is the weather today",
        .lang = "en",
        .confidence = 0.95F,
        .audio_duration = opts_.user_speech_duration,
        .processing_duration = opts_.stt_processing,
    });

    bus_.publish(event::LlmStarted{ .turn = turn });
    sleep_for_or_stop(opts_.llm_first_token_delay, running_);
    if (!running_.load(std::memory_order_acquire)) return;

    for (std::uint32_t s = 1; s <= opts_.sentences_per_turn; ++s) {
        // Sentence is "produced" by the LLM
        bus_.publish(event::LlmSentence{
            .turn = turn,
            .seq = s,
            .text = fmt::format("This is synthetic sentence {} of turn {}.", s, turn),
            .lang = "en",
        });
        // TTS turns the sentence into audio
        sleep_for_or_stop(opts_.tts_first_audio, running_);
        if (!running_.load(std::memory_order_acquire)) return;
        bus_.publish(event::TtsStarted{ .turn = turn, .seq = s });
        bus_.publish(event::TtsAudioChunk{ .turn = turn, .seq = s, .bytes = 1024 });
        bus_.publish(event::TtsFinished{ .turn = turn, .seq = s });
        bus_.publish(event::PlaybackStarted{ .turn = turn, .seq = s });
        sleep_for_or_stop(opts_.playback_per_sentence, running_);
        if (!running_.load(std::memory_order_acquire)) return;
        bus_.publish(event::PlaybackFinished{ .turn = turn, .seq = s });

        sleep_for_or_stop(opts_.llm_per_sentence, running_);
        if (!running_.load(std::memory_order_acquire)) return;
    }

    bus_.publish(event::LlmFinished{
        .turn = turn,
        .cancelled = false,
        .tokens_generated = opts_.sentences_per_turn * 12,
    });
}

void FakeDriver::run_one_turn_with_barge_in(event::TurnId turn) {
    // Run the turn until the first sentence has played. Then fire
    // UserInterrupted while still Speaking. Skip the rest of the turn.
    if (!opts_.suppress_speech_events) {
        bus_.publish(event::SpeechStarted{
            .turn = turn,
            .ts = std::chrono::steady_clock::now(),
        });
    }
    sleep_for_or_stop(opts_.user_speech_duration, running_);
    if (!running_.load(std::memory_order_acquire)) return;

    if (!opts_.suppress_speech_events) {
        bus_.publish(event::SpeechEnded{ .turn = turn, .ts = std::chrono::steady_clock::now() });
    }
    sleep_for_or_stop(opts_.stt_processing, running_);
    if (!running_.load(std::memory_order_acquire)) return;

    bus_.publish(event::FinalTranscript{
        .turn = turn,
        .text = "longer question please answer at length",
        .lang = "en",
        .confidence = 0.9F,
        .audio_duration = opts_.user_speech_duration,
        .processing_duration = opts_.stt_processing,
    });
    bus_.publish(event::LlmStarted{ .turn = turn });
    sleep_for_or_stop(opts_.llm_first_token_delay, running_);
    if (!running_.load(std::memory_order_acquire)) return;

    // First sentence + playback (so the outcome is "interrupted" not "discarded").
    bus_.publish(event::LlmSentence{
        .turn = turn, .seq = 1,
        .text = "I am about to be cut off mid-sentence by an impatient user.",
        .lang = "en",
    });
    sleep_for_or_stop(opts_.tts_first_audio, running_);
    bus_.publish(event::TtsStarted{ .turn = turn, .seq = 1 });
    bus_.publish(event::TtsAudioChunk{ .turn = turn, .seq = 1, .bytes = 1024 });
    bus_.publish(event::TtsFinished{ .turn = turn, .seq = 1 });
    bus_.publish(event::PlaybackStarted{ .turn = turn, .seq = 1 });
    sleep_for_or_stop(opts_.playback_per_sentence, running_);
    bus_.publish(event::PlaybackFinished{ .turn = turn, .seq = 1 });

    // Now barge in.
    bus_.publish(event::UserInterrupted{
        .turn = turn,
        .ts = std::chrono::steady_clock::now(),
    });

    // Late-arriving events from the cancelled turn — these should be
    // rejected by the FSM (turn id mismatch).
    bus_.publish(event::LlmSentence{
        .turn = turn, .seq = 2, .text = "stale sentence", .lang = "en",
    });
    bus_.publish(event::PlaybackFinished{ .turn = turn, .seq = 2 });
    bus_.publish(event::LlmFinished{
        .turn = turn,
        .cancelled = true,
        .tokens_generated = 12,
    });
}

} // namespace acva::pipeline
