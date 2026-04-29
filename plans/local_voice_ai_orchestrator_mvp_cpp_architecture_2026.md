# Local Voice AI Orchestrator MVP (C++)

## Goal

Local production-grade configurable voice assistant for long conversations using local models on RTX 4060.

## High-Level Pipeline

Mic → AEC/NS → VAD → STT → Dialogue Manager → LLM → TTS → Speaker

## Core Principles

- C++ orchestrator as central runtime
- External inference services for models
- Event-driven architecture
- Strong cancellation semantics
- YAML-configurable pipeline
- SQLite-backed session memory

## Main Components

### Audio Layer

- Audio capture backend: PortAudio / RtAudio
- Resampler
- Ring buffers
- Playback engine
- Device abstraction

### Audio Cleanup

- WebRTC Audio Processing Module
- Acoustic Echo Cancellation (AEC)
- Noise Suppression (NS)
- Automatic Gain Control (AGC)

### VAD Layer

- Silero VAD
- Optional WebRTC VAD fast gate

### STT Layer

- whisper.cpp external service
- Utterance-based transcription
- Optional streaming partial transcription later

### LLM Layer

- llama.cpp server
- Qwen2.5-7B-Instruct Q4\_K\_M / Q5\_K\_M
- Streaming generation
- OpenAI-compatible REST API

### TTS Layer

- Piper external service
- Sentence-level streaming synthesis

### Memory Layer

- SQLite storage
- Recent turns
- Session summaries
- Facts store

## Internal Event Types

- AudioFrame
- SpeechStarted
- SpeechEnded
- PartialTranscript
- FinalTranscript
- LlmToken
- LlmSentence
- TtsAudioChunk
- UserInterrupted
- CancelGeneration
- MemoryUpdate
- ErrorEvent

## Dialogue State Machine

Idle → Listening → UserSpeaking → Transcribing → Thinking → Speaking → Interrupted → Listening

## Barge-in Rules

If user speech starts during assistant playback:

- stop playback immediately
- cancel TTS
- cancel LLM generation
- switch to listening

## Config Example

```yaml
pipeline:
  nodes:
    - audio_capture
    - vad
    - stt
    - dialogue
    - llm
    - tts
    - playback

llm:
  provider: openai_compatible
  base_url: http://127.0.0.1:8080/v1
  model: qwen2.5-7b-instruct

stt:
  provider: whisper_cpp

vad:
  provider: silero

memory:
  backend: sqlite
```

## Recommended Tech Stack

- C++23
- Boost.Asio / Cobalt
- SPSC queues
- yaml-cpp
- SQLite
- glaze / nlohmann-json
- boost::beast / cpp-httplib

## Development Phases

### Phase 1 (2–4 weeks)

- Basic pipeline
- VAD + STT + llama.cpp + TTS
- No robust interruption yet

### Phase 2 (8–12 weeks)

- Barge-in
- Streaming TTS
- Stable memory
- Logs / metrics

### Phase 3 (4–6 months)

- Full audio robustness
- Wake word
- Tool calling
- GUI
- Packaging

## Main Technical Risks

- Echo cancellation complexity
- Latency accumulation
- Cancellation races
- Memory drift
- Audio device instability

## Success Criteria for MVP

- End-to-end latency under 2 sec
- Stable 30+ min dialogue
- Reliable interruption
- No crashes during multi-hour sessions


---

# Architecture Review Addendum

## 1. MVP Scope Boundary

The MVP should be production-grade in orchestration, cancellation, observability, and configuration, but should avoid prematurely embedding every model runtime directly into the C++ process.

### In Scope

- Local voice conversation runtime
- Configurable pipeline graph
- External model services
- Streaming LLM output
- Sentence-level TTS scheduling
- Barge-in support
- SQLite-backed memory
- Logs, metrics, traces
- Restartable service supervision

### Out of Scope for First MVP

- Fully speech-native end-to-end model
- Custom STT model training
- Custom TTS model training
- Multi-user server deployment
- Mobile deployment
- Advanced emotional prosody
- Perfect full-duplex conversation

## 2. Runtime Topology

Recommended MVP topology:

```text
+-------------------------+
| C++ Orchestrator        |
|                         |
| Audio I/O               |
| VAD                     |
| Dialogue FSM            |
| Event Bus               |
| Memory                  |
| Supervisor              |
+-----------+-------------+
            |
            | HTTP / IPC
            |
+-----------+-------------+
| llama.cpp server        |
+-------------------------+

+-------------------------+
| whisper.cpp service     |
+-------------------------+

+-------------------------+
| Piper TTS service       |
+-------------------------+
```

### Why External Services First

- Easier crash isolation
- Easier backend replacement
- Easier logging and profiling
- Avoids GPU/runtime conflicts inside one process
- Allows independent service restart
- Keeps C++ orchestrator focused on real-time control logic

Embedding llama.cpp, whisper.cpp, or Piper can be considered later after the pipeline behavior is stable.

## 3. Node Lifecycle

Every pipeline node should follow a predictable lifecycle:

```text
Created → Configured → Started → Running → Draining → Stopped → Destroyed
```

### Required Node Operations

```cpp
struct Node {
    virtual std::string name() const = 0;
    virtual Task<void> configure(const NodeConfig&) = 0;
    virtual Task<void> start(NodeContext&) = 0;
    virtual Task<void> stop(StopReason) = 0;
    virtual Task<void> drain() = 0;
    virtual NodeHealth health() const = 0;
};
```

### Stop Reasons

- Normal shutdown
- User interruption
- Backend failure
- Config reload
- Timeout
- Supervisor restart

## 4. Event Bus Design

The event bus should support typed events, bounded queues, and cancellation-aware consumers.

### Event Classes

```text
Audio events     - AudioFrame, PlaybackFrame, AudioDeviceChanged
Speech events    - SpeechStarted, SpeechEnded, VadProbability
STT events       - PartialTranscript, FinalTranscript
LLM events       - LlmStarted, LlmToken, LlmSentence, LlmFinished
TTS events       - TtsStarted, TtsAudioChunk, TtsFinished
Control events   - CancelGeneration, UserInterrupted, Pause, Resume
Memory events    - MemoryLookup, MemoryUpdate, SummaryUpdated
System events    - ErrorEvent, HealthChanged, MetricsSample
```

### Queue Policy

Different queues need different policies:

| Queue | Type | Policy |
|---|---|---|
| Audio capture → VAD | realtime | bounded, drop oldest on overflow |
| VAD → STT | segment | bounded, never drop completed utterance unless cancelled |
| STT → Dialogue | text | bounded, preserve order |
| LLM → TTS | text stream | cancellable, preserve order |
| TTS → Playback | audio | bounded, low-latency drain on interruption |
| Metrics/logs | telemetry | lossy allowed |

## 5. Threading / Executor Model

Recommended model:

```text
Audio callback thread
  ↓ lock-free SPSC
Audio processing executor
  ↓
Dialogue executor
  ↓
Network I/O executor
  ↓
Playback executor
```

### Rules

- Never block inside audio callback
- Never do HTTP calls from audio thread
- Never allocate heavily in realtime audio path
- Use bounded queues for every async boundary
- Cancellation must propagate top-down
- Metrics/logging must not block critical path

## 6. Cancellation Graph

Barge-in requires structured cancellation.

```text
UserInterrupted
  ├─ cancel current LLM stream
  ├─ cancel pending TTS requests
  ├─ flush playback queue
  ├─ mark assistant turn as interrupted
  └─ switch dialogue FSM to Listening/UserSpeaking
```

### Assistant Turn States

```text
NotStarted
Generating
Speaking
Interrupted
Completed
Discarded
```

On interruption, the system should decide whether to keep partial assistant text in memory.

Recommended MVP policy:

```text
If assistant was interrupted before first full sentence: discard.
If assistant spoke at least one full sentence: store as interrupted assistant turn.
```

## 7. Backpressure Strategy

Backpressure is mandatory for long sessions.

### Sources of Pressure

- STT slower than realtime
- LLM slow first token
- TTS slower than LLM generation
- Playback queue grows too large
- Memory summarization runs too often

### Required Policies

- Limit max utterance duration
- Limit max queued TTS sentences
- Limit max assistant response length in voice mode
- Stop LLM generation after enough spoken content unless user asks for detail
- Summarization should run outside the critical path

Example voice-mode limits:

```yaml
dialogue:
  max_user_utterance_sec: 60
  max_assistant_sentences: 6
  max_tts_queue_sentences: 3
  interrupt_on_user_speech: true
  summarize_async: true
```

## 8. Dialogue Manager Responsibilities

The Dialogue Manager owns conversational state.

It should:

- Build LLM prompt from memory and recent turns
- Enforce spoken-answer style
- Split LLM output into TTS-ready sentences
- Handle interruption
- Decide what enters memory
- Decide when to summarize
- Apply max response limits
- Manage tool calls later

It should not:

- Directly manage audio devices
- Directly perform model inference
- Block on long memory operations

## 9. Prompt Policy for Voice Mode

Voice mode should have a different prompt style from text chat.

Recommended system policy:

```text
You are a local voice assistant.
Answer in short spoken paragraphs.
Prefer direct answers over long lists.
Ask at most one clarification question.
When interrupted, stop and adapt to the new user request.
For technical topics, give concise but precise explanations.
```

For this user profile, technical explanations can be dense, but voice responses should still avoid huge enumerations unless explicitly requested.

## 10. Memory Architecture

### SQLite Tables

```sql
sessions(id, started_at, ended_at, title)
turns(id, session_id, role, text, started_at, ended_at, status)
summaries(id, session_id, range_start_turn, range_end_turn, summary)
facts(id, key, value, source_turn_id, confidence, updated_at)
settings(key, value, updated_at)
```

### Prompt Assembly

```text
System prompt
+ user preferences
+ durable facts
+ session summary
+ last N turns verbatim
+ current user turn
```

### Memory MVP Policy

- Keep last 8–12 turns verbatim
- Summarize older turns every 10–20 turns
- Extract only stable technical preferences/facts
- Do not store noisy interrupted fragments unless useful

## 11. Service Supervision

The orchestrator should supervise external services.

### Per-Service State

```text
NotConfigured
Starting
Healthy
Degraded
Unhealthy
Restarting
Disabled
```

### Health Checks

- llama.cpp: HTTP `/health` or lightweight completion
- whisper.cpp: test endpoint / process liveness
- Piper: synthesize short test phrase or process liveness

### Restart Policy

```yaml
supervisor:
  restart_backoff_ms: [500, 1000, 2000, 5000]
  max_restarts_per_minute: 5
  fail_pipeline_if_llm_down: true
  fail_pipeline_if_stt_down: true
  allow_tts_disabled: true
```

## 12. Observability

Production-grade MVP needs observability from day one.

### Logs

Structured logs:

```json
{
  "ts": "...",
  "level": "info",
  "component": "dialogue",
  "event": "llm_first_token",
  "turn_id": 42,
  "latency_ms": 381
}
```

### Metrics

Minimum metrics:

- End-of-turn latency
- STT latency
- LLM first-token latency
- LLM tokens/sec
- TTS first-audio latency
- Playback underruns
- VAD false starts
- Interruption count
- Queue sizes
- Memory summarization time

### Tracing

Each user turn should have a trace ID:

```text
turn_id=42
  audio_segment
  vad_end
  stt_start
  stt_final
  llm_start
  first_token
  first_sentence
  tts_start
  first_audio
  playback_start
```

## 13. Error Recovery

### Recoverable Errors

- TTS failure for one sentence
- LLM stream timeout
- STT service restart
- Audio device reconnect
- Memory write failure

### Non-Recoverable Errors

- No input device
- No output device
- LLM backend unavailable
- Config invalid at startup

### User-Facing Behavior

For local voice UX, errors should be spoken only when useful.

Example:

```text
"I lost the speech engine for a moment. Please repeat that."
```

Avoid verbose technical error speech unless debug mode is enabled.

## 14. Configuration System

### Requirements

- YAML config
- Schema validation
- Clear startup errors
- Hot reload for safe fields
- No hot reload for unsafe fields unless pipeline restarts

### Hot-Reloadable

- LLM temperature
- Max response length
- VAD threshold
- TTS speed
- Logging level

### Restart-Required

- Audio device backend
- Sample rate
- Model service endpoint
- Pipeline graph structure

## 15. Testing Strategy

### Unit Tests

- Dialogue FSM transitions
- Sentence splitter
- Prompt builder
- Memory summarizer policy
- Cancellation propagation
- Config validation

### Integration Tests

- Fake STT → fake LLM → fake TTS
- Real llama.cpp smoke test
- Real whisper.cpp smoke test
- TTS playback queue cancellation

### Audio Tests

- Recorded utterance fixtures
- Noise fixtures
- Echo fixtures
- Silence detection tests
- Long-session soak test

### Soak Test Target

```text
4-hour local session
no crash
no unbounded memory growth
no queue growth
stable latency percentiles
```

## 16. Latency Budget

Target production-grade MVP:

| Stage | Target |
|---|---:|
| VAD end-of-turn delay | 300–700 ms |
| STT final transcript | 200–800 ms |
| Prompt assembly | <50 ms |
| LLM first token | 300–1000 ms |
| First sentence ready | 500–1500 ms |
| TTS first audio | 100–500 ms |
| Playback start | <100 ms |

Good perceived latency:

```text
~1–2 seconds after user stops speaking
```

## 17. Milestones

### Milestone 0: Skeleton Runtime

- C++ app starts
- YAML config loads
- Event bus works
- Logs and metrics work
- Fake nodes connected

### Milestone 1: Text-Only LLM Runtime

- Dialogue Manager
- llama.cpp streaming
- prompt assembly
- memory writes

### Milestone 2: Voice Input

- Audio capture
- VAD
- utterance segmentation
- whisper.cpp transcription

### Milestone 3: Voice Output

- Piper TTS
- playback queue
- sentence splitting
- streaming LLM → TTS bridge

### Milestone 4: Barge-In

- detect speech during playback
- stop playback
- cancel LLM/TTS
- resume listening

### Milestone 5: Production MVP Hardening

- service supervision
- restart policy
- health checks
- soak tests
- metrics dashboard
- config validation

## 18. Recommended Initial Implementation Order

1. Fake pipeline with typed events
2. Dialogue FSM
3. llama.cpp integration
4. SQLite memory
5. TTS playback with Piper
6. Audio capture + VAD
7. whisper.cpp transcription
8. End-to-end pipeline
9. Barge-in
10. AEC / noise suppression
11. Service supervision
12. Soak testing

## 19. Key Architectural Decision

The most important early decision:

```text
Keep the orchestrator responsible for timing, cancellation, state, and memory.
Keep model runtimes external until the control plane is mature.
```

This minimizes complexity while keeping the system customizable and production-oriented.

