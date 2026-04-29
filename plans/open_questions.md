# Open Questions

Decisions to make before or during implementation. Grouped by area. Each question notes the default assumption used in `project_design.md` so we can proceed if the question stays open, plus what specifically would change if the answer differs.

---

## A. Hardware & Deployment

**A1. Headphones or speakers as the default UX?**
- Default assumption: support both, but headphone mode is the reliable mode until M6 (AEC) lands.
- Impact: if speaker-only is required from day one, M6 must move before M7 *and* the latency budget for AEC delay estimation must be tight. If headphones-only is acceptable, AEC can be a stretch goal.

**A2. Single-machine deployment only, or multiple workstations?**
- Default assumption: single machine. All services on `127.0.0.1`.
- Impact: cross-machine deployment changes service discovery, auth, and TLS posture. Would also reopen the "is HTTP the right IPC?" question.

**A3. Background usage during active GPU workloads (gaming, video work)?**
- Default assumption: voice assistant has exclusive GPU access while running.
- Impact: graceful degradation under VRAM pressure (offload Whisper to CPU dynamically? quantize down?) is not in the design today.

---

## B. Models & Inference

**B1. Whisper on GPU or CPU?**
- Default assumption: CPU (Whisper small or medium-CPU). Saves VRAM for Qwen.
- Impact: GPU Whisper is faster (sub-200 ms STT on small) but eats ~1 GB VRAM. Decide based on measured first-token P95 budget pressure.

**B2. Qwen2.5-7B Q4_K_M vs Q5_K_M vs Q4_K_S?**
- Default assumption: Q4_K_M (~4.5 GB).
- Impact: Q5_K_M is higher quality but ~5.4 GB; Q4_K_S faster cold-start but lower quality. Should be a config flag with a benchmark-driven default.

**B3. Whisper model size: tiny / base / small / medium?**
- Default assumption: small (en) or small (multilingual) depending on B6.
- Impact: tiny is fast but error-prone; medium is accurate but slow. Ties into latency budget.

**B4. Piper voice model selection?**
- Default assumption: one English voice, configurable.
- Impact: voice quality is purely UX; pick something pleasant and document how to swap.

**B5. Streaming partial STT — defer to post-MVP?**
- Default assumption: utterance-based for MVP; partial transcription is post-MVP.
- Impact: partial STT enables overlapping STT/LLM ("speculative thinking") and shaves ~400 ms; complicates cancellation. Worth a follow-up.

**B6. English-only or multilingual?**
- Default assumption: English-only for MVP.
- Impact: multilingual changes Whisper model choice, Piper voice catalog, and prompt policy.

---

## C. Audio

**C1. Resampler choice: libsamplerate, speex resampler, or soxr?**
- Default assumption: libsamplerate (`SRC_SINC_FASTEST` for realtime paths, `SRC_SINC_MEDIUM_QUALITY` for playback).
- Impact: speex is lighter, soxr is highest quality. One choice for the project.

**C2. Audio backend: PortAudio or RtAudio?**
- Default assumption: PortAudio.
- Impact: minor; both work. Only matters if a target distro has packaging issues with one.

**C3. AEC reference signal: post-resampler loopback tap, or virtual loopback device (PulseAudio/PipeWire monitor)?**
- Default assumption: in-process post-resampler tap.
- Impact: virtual loopback is simpler to wire but adds OS-level latency variability; in-process tap is more reliable but requires careful frame alignment.

**C4. VAD hangover defaults: 600 ms?**
- Default assumption: 600 ms hangover, 200 ms minimum utterance.
- Impact: lower hangover = snappier turn-taking but more interruptions of slow speakers. Should expose as config and tune on real data.

**C5. Wake-word vs always-on VAD vs push-to-talk for Phase 1?**
- Default assumption: always-on VAD, with manual mute hotkey.
- Impact: always-on has false-trigger issues with TV/music in the room. PTT is more reliable but worse UX. Wake-word is Phase 3.

**C6. Sample rate for capture and playback?**
- Default assumption: 48 kHz hardware, 16 kHz internal pipeline (post-resample).
- Impact: 44.1 kHz hardware also possible. Both should work but require explicit config.

---

## D. Concurrency & I/O

**D1. asio: standalone or Boost.Asio?**
- Default assumption: standalone asio (one less Boost dependency).
- Impact: minor. Boost.Asio is identical API but pulls Boost.

**D2. SSE client: libcurl directly, or a thin C++ wrapper (cpr)?**
- Default assumption: libcurl directly.
- Impact: cpr is friendlier but adds dependency. Direct libcurl is fine for our use.

**D3. Lock-free SPSC ring: hand-roll, or a library (boost::lockfree, moodycamel)?**
- Default assumption: hand-roll a simple SPSC ring (atomic head/tail, fixed size). It's ~100 lines.
- Impact: minor. Library use is fine if dependency cost is acceptable.

**D4. JSON library: nlohmann or glaze?** *(resolved)*
- Decision: glaze, used for both JSON and YAML.
- Rationale: one library, reflective struct binding, fast. Trade-off accepted: newer than nlohmann, smaller ecosystem.

---

## E. Memory & Persistence

**E1. Should the orchestrator record raw audio per turn?**
- Default assumption: off by default; configurable.
- Impact: privacy, disk space, and a useful debugging tool. If on, decide retention policy (rolling N hours? per session? wipe on session end?).

**E2. Encryption-at-rest for SQLite?**
- Default assumption: none (local single-user machine).
- Impact: SQLCipher adds dependency and complexity. Worth it only if user wants confidentiality from local attackers.

**E3. Session boundary: when does a new session start?**
- Default assumption: explicit user command, or after N minutes of idle.
- Impact: too-frequent session breaks fragment memory; never-ending sessions accumulate noise.

**E4. Facts table: who decides what's a "stable preference"?**
- Default assumption: a separate, conservative LLM extraction prompt run on summarization boundaries.
- Impact: aggressive extraction → noise; conservative → useful facts get missed. Needs offline evaluation.

**E5. Summary regeneration: when?**
- Default assumption: when source-hash mismatches, or every N new turns since last summary.
- Impact: too frequent → wastes LLM cycles; too rare → stale summary. Tune empirically.

**E6. Memory bootstrap on first launch — empty, or load preferences from a YAML?**
- Default assumption: empty; user state grows over time.
- Impact: a YAML "user profile" seed makes first-session behavior much better. Could be added without changing schema (just pre-populate `facts` and `settings`).

---

## F. Dialogue & Prompting

**F1. System prompt: in code, or in config?**
- Default assumption: in YAML config so it's editable without rebuild.
- Impact: easy answer, just confirm.

**F2. Tool calling reservation: where does the hook go?**
- Default assumption: Dialogue Manager parses LLM output for tool-call markers before passing to SentenceSplitter; if a tool call is detected, suppress TTS and execute the tool. Phase 3.
- Impact: design must reserve a "non-spoken token segment" path now, even if unused. Requires deciding tool-call format (JSON in fenced block? OpenAI-compatible function calling?).

**F3. Maximum prompt length policy?**
- Default assumption: cap context at ~3000 tokens (system + facts + summary + last 10 turns + current). Hit ceiling → trigger summarization.
- Impact: directly drives LLM first-token latency. Tune against measured latency.

**F4. Should the assistant be allowed to ask clarifying questions, and how often?**
- Default assumption: at most one clarification per turn (per the system prompt).
- Impact: enforced via prompt only. Not enforced structurally; rely on Qwen's instruction-following.

**F5. Spoken-style enforcement: prompt only, or post-process LLM output?**
- Default assumption: prompt only for MVP.
- Impact: LLMs ignore "no lists" instructions sometimes. Post-processing (strip markdown bullets, collapse code fences to "I have a code snippet, want me to send it?") is robust but complex.

---

## G. Cancellation & Backpressure

**G1. On `UserInterrupted`, do we cancel the LLM HTTP request, or let it complete and discard?**
- Default assumption: actively cancel (libcurl abort).
- Impact: completing-and-discarding wastes GPU but is simpler. Aborting saves cycles but requires server-side support (llama.cpp does support client disconnect).

**G2. TTS request cancellation: cancel-in-flight, or wait + drop?**
- Default assumption: wait + drop. Piper synthesis is fast (sub-second per sentence); cancel mid-synthesis adds complexity without much benefit.
- Impact: under bursty interruptions, slight CPU waste. Acceptable for MVP.

**G3. Hard cap on assistant response length in voice mode?**
- Default assumption: 6 sentences default, configurable.
- Impact: too low → assistant feels truncated; too high → spoken responses become tedious.

---

## H. Observability & Operations

**H1. Metrics: Prometheus endpoint, or stdout dump on signal?**
- Default assumption: Prometheus on `/metrics` (HTTP, localhost).
- Impact: stdout-only is simpler but harder to graph. Prometheus is small dep (`prometheus-cpp`) and standard.

**H2. Tracing: in-process logs only, or OTLP export?**
- Default assumption: logs only for MVP. JSON logs are sufficient to reconstruct traces offline.
- Impact: OTLP would require an OTel collector. Not worth it for single-user local app.

**H3. Crash dump policy?**
- Default assumption: rely on systemd-coredump or equivalent; no custom signal handler.
- Impact: a custom handler that flushes the event ring before crash would be useful but is complex.

**H4. Single-binary distribution, or a service-set with a launcher?**
- Default assumption: single orchestrator binary; services launched as child processes via the supervisor with paths from config.
- Impact: simpler packaging; relies on user installing llama.cpp/whisper.cpp/Piper separately. A bundled distribution is post-MVP.

**H5. How are external services started initially — by orchestrator, by systemd, or by user?**
- Default assumption: orchestrator launches them as supervised child processes.
- Impact: systemd integration would be cleaner on Linux but couples the design to systemd. Orchestrator-launched is portable.

---

## I. UX & Control Plane

**I1. Is there a GUI for MVP, or CLI/headless only?**
- Default assumption: headless. Optional minimal HTTP control plane (`/reload`, `/wipe`, `/status`).
- Impact: GUI is Phase 3.

**I2. Mute / push-to-talk hotkey: where does it come from?**
- Default assumption: no hotkey for MVP; a `/mute` HTTP endpoint and a config-toggleable behavior.
- Impact: a system-wide hotkey requires X11/Wayland-specific code. Out of scope.

**I3. Error feedback to the user: spoken, written, or both?**
- Default assumption: spoken for unrecoverable errors only ("I lost the speech engine, please repeat"); written to logs always.
- Impact: too talkative on errors is annoying; too silent is confusing.

**I4. Conversation export: text only, or audio + text?**
- Default assumption: text export from `turns` table via CLI.
- Impact: audio export requires E1 to be enabled.

---

## J. Testing & Validation

**J1. Soak test environment: developer machine, or CI?**
- Default assumption: developer machine; CI runs only short integration tests with fake services.
- Impact: a 4-hour soak in CI is impractical; a nightly run on a dedicated box is the right answer eventually.

**J2. Audio fixture corpus: where does it come from?**
- Default assumption: hand-recorded by the developer for MVP; expand to LibriSpeech-style fixtures later.
- Impact: small fixture set risks overfitting VAD/STT thresholds. Document this limitation.

**J3. Latency-regression detection?**
- Default assumption: log P50/P95 in soak test summary; manual review.
- Impact: an automated regression gate would catch drift early; out of scope for MVP.

---

## K. Strategic / Future-Proofing

**K1. When (if ever) do we move from external services to embedded model runtimes?**
- Default assumption: revisit after Milestone 8 once control plane is stable. May never be worth it.
- Impact: low-priority decision; flagged so we don't accidentally do it early.

**K2. How does this design extend to a streaming end-to-end speech model (e.g., Qwen-Audio, future models)?**
- Default assumption: such a model would replace the STT+LLM stages, keeping VAD/Dialogue/TTS/Playback. The Dialogue Manager's responsibilities don't change.
- Impact: the design is compatible. No action now.

**K3. Multi-turn tool use (web fetch, file read, code execution) — security model?**
- Default assumption: out of scope for MVP. When added, tool execution must be sandboxed and user-approved per call.
- Impact: shapes Phase 3 architecture; reserve a `ToolExecutor` component slot now.
