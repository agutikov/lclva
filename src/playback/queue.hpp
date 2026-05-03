#pragma once

#include "dialogue/turn.hpp"
#include "event/event.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace acva::playback {

// One unit of synthesized audio handed off from the TTS bridge to the
// playback engine. Tagged with `(turn, seq)` so a barge-in can drop
// stale chunks before the audio callback ever consumes them.
//
// `samples` is mono int16 PCM at the device sample rate (resampled
// upstream by audio::Resampler). Chunk size is producer-defined; the
// playback engine handles arbitrary lengths and assembles its own
// fixed-size frames for PortAudio.
struct AudioChunk {
    dialogue::TurnId  turn = event::kNoTurn;
    event::SequenceNo seq  = 0;
    std::vector<std::int16_t> samples;
    // True on the LAST chunk a producer (TtsBridge) enqueues for a
    // given (turn, seq) — i.e., the trailing edge of a sentence. The
    // PlaybackEngine fires `PlaybackFinished` once per consumed
    // end-of-sentence chunk; non-terminal chunks just increment the
    // chunks-played counter. The producer always enqueues a final
    // chunk with this flag, even if `samples` is empty, so engines
    // can rely on a deterministic per-sentence signal.
    bool end_of_sentence = false;
};

// Bounded FIFO between the TTS bridge (producer) and the playback
// engine (consumer).
//
// Threading contract:
//   • One producer thread (TtsBridge I/O thread) calls enqueue().
//   • One consumer thread (PortAudio callback) calls dequeue_active().
//   • The barge-in handler (FSM thread) calls invalidate_before().
//   • size() / drops() / enqueued() / dequeued() are safe to call
//     from any thread for /metrics and /status.
//
// The queue uses a mutex with short critical sections (push_back,
// pop_front, deque rebuild). The audio callback is not strictly
// lock-free, but each operation is bounded and contention is rare —
// the producer enqueues at most a few times per sentence (~10 ms
// chunks) and the consumer dequeues at the same cadence. The
// "lock-free against the audio thread" target from project_design
// §4.10 is achieved at a higher level by the M4 SPSC ring that the
// PortAudio callback drains directly.
//
// On capacity overflow the queue rejects the new chunk (DropNewest);
// the producer is expected to backpressure rather than overrun. This
// matches `dialogue.max_tts_queue_sentences` — the bridge stops
// pulling more sentences off the LLM stream once depth crosses that
// threshold. The hard cap here is just a safety net.
class PlaybackQueue {
public:
    explicit PlaybackQueue(std::size_t max_chunks);

    PlaybackQueue(const PlaybackQueue&)            = delete;
    PlaybackQueue& operator=(const PlaybackQueue&) = delete;
    PlaybackQueue(PlaybackQueue&&)                 = delete;
    PlaybackQueue& operator=(PlaybackQueue&&)      = delete;

    // Producer side. Returns true if accepted, false if the queue is
    // full or the chunk's turn has already been invalidated. A `false`
    // return increments drops().
    bool enqueue(AudioChunk chunk);

    // Backpressure variant for chunks that MUST be enqueued — most
    // notably the `end_of_sentence` marker, whose loss strands the
    // FSM in Speaking. Blocks until the queue has room or the
    // cancellation token flips. Returns true on enqueue success,
    // false if cancelled. Polls every `poll` ms; the audio cb's
    // dequeue cadence (≈10 ms / chunk) means the wait is short in
    // practice. No drop counted on success regardless of how long
    // the caller waited.
    bool enqueue_blocking(
        AudioChunk chunk,
        const std::shared_ptr<dialogue::CancellationToken>& cancel,
        std::chrono::milliseconds poll = std::chrono::milliseconds(5));

    // Consumer side. Returns the next chunk where `chunk.turn ==
    // active_turn`. Stale chunks at the head (turn != active_turn)
    // are dropped and counted before the matching chunk is returned.
    // Returns nullopt if the queue is empty or only stale chunks
    // remain after head-skipping.
    std::optional<AudioChunk> dequeue_active(dialogue::TurnId active_turn);

    // Drop every chunk whose turn id is less than `next_turn`. Called
    // by the FSM/bridge on UserInterrupted to drain the previous
    // turn's audio without waiting for the audio callback to walk the
    // head one chunk at a time. Returns the number of chunks dropped.
    std::size_t invalidate_before(dialogue::TurnId next_turn);

    // Drop every chunk regardless of turn. Used at shutdown and by
    // tests; production code prefers invalidate_before().
    std::size_t clear();

    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] std::size_t capacity() const noexcept { return cap_; }

    // Total int16 samples in the queue tagged with `turn`. Used by the
    // PlaybackEngine's pre-buffer threshold to decide when to start
    // draining a streaming TTS turn — chunk count is unreliable
    // because each libcurl-burst-derived chunk has different size.
    // Linear in queue depth (bounded by cap_).
    [[nodiscard]] std::size_t pending_samples_for(dialogue::TurnId turn) const;
    [[nodiscard]] std::uint64_t drops() const noexcept    { return drops_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t enqueued() const noexcept { return enqueued_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t dequeued() const noexcept { return dequeued_.load(std::memory_order_relaxed); }

private:
    mutable std::mutex mu_;
    std::deque<AudioChunk> q_;
    const std::size_t cap_;

    std::atomic<std::uint64_t> drops_{0};
    std::atomic<std::uint64_t> enqueued_{0};
    std::atomic<std::uint64_t> dequeued_{0};
};

} // namespace acva::playback
