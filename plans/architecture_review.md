# Architectural Analysis: Local Voice AI Orchestrator MVP

## Overall Assessment

This is a **well-structured, production-oriented MVP design** (Grade: A-). The author correctly identifies the hard problems—cancellation semantics, backpressure, latency budgeting, and state management—while making pragmatic scope boundaries.

## Key Strengths

1. **Clean Separation of Concerns**
   Externalizing `llama.cpp`, `whisper.cpp`, and `Piper` while keeping the C++ orchestrator focused on **timing, cancellation, state, and memory** is architecturally sound. It prevents GPU/runtime conflicts and allows independent service lifecycle management.

2. **Explicit Cancellation Graph**
   Section 6 correctly models barge-in as structured cancellation across the LLM stream, TTS requests, playback queue, dialogue FSM, and memory policy. This is the most underestimated part of voice systems and is handled well here.

3. **Event Bus with Queue Policies**
   The queue policy table (Section 4) differentiates `realtime` (drop oldest), `segment` (never drop utterance), and `telemetry` (lossy allowed)—exactly the right level of detail.

4. **Node Lifecycle Model**
   The `Created → Configured → Started → Running → Draining → Stopped → Destroyed` lifecycle with explicit `drain()` and `health()` operations will prevent resource leaks and simplify testing.

5. **Observability-First Design**
   Structured logs, metrics, and per-turn tracing from day one (Section 12) is mandatory for latency debugging in real-time systems.

## Areas of Concern & Gaps

1. **Boost.Asio / Cobalt Risk**
   Recommending **Boost.Asio / Cobalt** for C++23 coroutines is a medium-risk dependency. Boost.Cobalt is relatively new, C++23 coroutine support varies across compilers, and debugging coroutine stack traces is notoriously difficult.
   - **Mitigation**: Pin the version and keep a thread-pool fallback for the audio path.

2. **AEC / NS Sequencing**
   AEC/NS is placed in Phase 3, but barge-in (Phase 2) requires echo cancellation to avoid the assistant's own TTS triggering VAD false positives.
   - **Recommendation**: Move basic AEC/AGC to Phase 2, or accept that early barge-in only works with headphones.

3. **Missing Audio Clock Synchronization**
   No mention of **clock drift** between capture and playback devices. On 30+ minute sessions, drift will cause AEC misalignment.
   - **Recommendation**: Add a `MonotonicAudioClock` abstraction referenced by both capture and playback.

4. **VAD → STT Handoff Ambiguity**
   The document does not specify how audio buffers are passed—accumulated ring buffer? overlap padding? who owns the buffer if STT is slower than realtime?
   - **Recommendation**: Define `SpeechEnded` to include a reference-counted audio buffer slice with explicit padding rules (e.g., 300ms before/after speech).

5. **Memory Summarization Blocking Risk**
   Section 8 says the Dialogue Manager should not block on long memory operations, but Section 10 shows summarization is part of prompt assembly. If summarization runs on the Dialogue executor, it blocks the FSM.
   - **Recommendation**: Make summarization fully async with a cached summary refreshed in the background.

6. **TTS Streaming Granularity Mismatch**
   Piper typically operates on full sentences, not tokens. The latency budget of 100–500ms for TTS first audio assumes sentence-level scheduling, but the LLM→TTS bridge needs a `SentenceSplitter` component.
   - **Recommendation**: Add an explicit `SentenceSplitter` to the Dialogue Manager with unit-tested boundary detection.

7. **Missing Model Warm-Up / Keep-Alive**
   `llama.cpp server` has cold-start latency after idle periods. The 300–1000ms first-token target may regress without a keep-alive strategy.
   - **Recommendation**: Add a supervisor keep-alive probe or configure `llama.cpp` to prevent model offload.

8. **HTTP Client Choice Ambiguity**
   The document lists both `boost::beast` and `cpp-httplib`. For simple blocking REST calls to `llama.cpp` and `Piper`, `cpp-httplib` is lighter. For streaming SSE, `beast` or `libcurl` is needed.
   - **Recommendation**: Use `cpp-httplib` for health checks and non-streaming endpoints; use `libcurl` or `beast` only for LLM SSE streams.

## Risk Matrix

| Risk | Severity | Mitigation |
|---|---|---|
| Boost.Cobalt maturity | Medium | Pin version; thread-pool fallback for audio path |
| AEC without hardware loopback | High | Prioritize headphone UX; delay speaker barge-in |
| TTS queue overflow during long LLM outputs | Medium | Enforce `max_tts_queue_sentences` with hard limit |
| Cancellation races between TTS and Playback | High | Use sequence numbers; reject stale audio chunks |
| SQLite blocking on memory writes | Low | Use WAL mode; offload to separate thread |
| VAD false triggers from TTS echo | High | Require AEC before reliable speaker-mode barge-in |

## Recommended Priority Adjustments

1. **Move AEC/AGC to Phase 2** (parallel with barge-in), even if it's a basic WebRTC APM integration.
2. **Add a `SentenceSplitter` component** to the Dialogue Manager explicitly, with unit tests.
3. **Define the `AudioBuffer` ownership model** between VAD and STT using reference-counted slices.
4. **Add a `ModelKeepAlive` policy** to the supervisor for `llama.cpp`.
5. **Clarify the HTTP client strategy**—use `cpp-httplib` for simple calls, `libcurl` for SSE streaming.

## Verdict

The architecture is **implementable and production-oriented**. With the adjustments above—particularly around AEC sequencing, audio buffer ownership, and async summarization—this design will produce a stable local voice assistant capable of the targeted 30+ minute sessions.
