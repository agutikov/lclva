# Should ACVA Be Rewritten Completely in Rust?

## Short Answer

No. ACVA should not be completely rewritten in Rust at this stage.

Rust would bring real advantages: stronger memory safety, stronger data-race prevention, a modern package ecosystem, excellent async tooling, and a good long-term fit for reliable systems software. Those benefits are meaningful for a voice orchestrator with realtime audio, queues, cancellation, persistence, service supervision, and long-running sessions.

However, a full rewrite would also discard working C++ code, delay the roadmap, force re-solving already-settled integration problems, introduce new FFI and packaging risks, and weaken alignment with the current design stack. The current architecture is explicitly C++23-based, already implemented across many modules, already tested, and already uses process isolation for the highest-risk model runtimes.

The better decision is:

> Keep the core MVP in C++23. Do not rewrite the project completely in Rust. Consider Rust later only for isolated post-MVP services or tools with clear boundaries.

---

## Context: What Exists Today

ACVA is not an early empty prototype. It already has a C++ project structure, CMake build, tests, and implemented modules.

The documented project design defines ACVA as a local voice AI orchestrator. The orchestrator owns:

- audio I/O;
- realtime audio handoff;
- VAD and endpointing;
- dialogue state;
- turn cancellation;
- LLM streaming coordination;
- TTS coordination;
- playback queueing;
- memory persistence;
- service health supervision;
- logging, metrics, and tracing.

The current repository reflects that design. The root build declares a C++ project, the source tree is C++, and the tests are C++. Rust is not currently part of the source tree, build graph, packaging, or design plan.

This means the real question is not whether Rust could implement ACVA. It could. The real question is whether rewriting the already-designed and partially implemented C++ project entirely in Rust is worth the disruption.

My answer is no.

---

## What Rust Would Improve

### 1. Memory Safety

Rust's strongest argument is memory safety. ACVA is a long-running local daemon-like program that handles audio buffers, queues, async callbacks, cancellation tokens, database writes, and service-client state. In C++, bugs in any of these areas can become use-after-free, double-free, iterator invalidation, dangling references, or undefined behavior.

Rust would make many classes of these bugs much harder to write:

- no use-after-free in safe Rust;
- no accidental data races in safe Rust;
- ownership and borrowing are checked by the compiler;
- thread-sharing rules are expressed through `Send` and `Sync`;
- resource cleanup is deterministic through RAII-like ownership.

For a system intended to run through multi-hour conversations, this is a serious advantage.

### 2. Concurrency Safety

ACVA has many concurrent boundaries:

- audio callback to audio processing;
- audio processing to STT;
- STT to dialogue;
- dialogue to LLM streaming;
- dialogue to TTS;
- TTS to playback;
- dialogue to memory thread;
- supervisor probes;
- HTTP control endpoints;
- metrics/logging paths.

Rust's type system is very good at making cross-thread ownership explicit. It would reduce the risk of accidentally sharing mutable state without synchronization.

This is especially relevant to:

- event bus subscribers;
- bounded queues;
- cancellation state;
- turn lifecycle state;
- playback queue invalidation;
- async service clients.

### 3. Modern Async and Service Tooling

Rust has excellent libraries for async service work:

- `tokio` for async runtime;
- `reqwest` or `hyper` for HTTP;
- `axum` for control APIs;
- `tracing` for structured spans;
- `serde` for JSON/YAML serialization;
- `sqlx` or `rusqlite` for SQLite;
- `prometheus` crates for metrics.

If ACVA were mostly a service coordinator with no realtime audio and few C/C++ dependencies, Rust would be a very attractive default.

### 4. Better Refactoring Guarantees

Rust's compiler gives stronger feedback during refactors. For a system with turn IDs, cancellation tokens, queue boundaries, and state machines, that matters.

Changing ownership relationships in C++ often relies on convention and tests. In Rust, many invalid ownership changes fail to compile.

### 5. Safer Parser and Protocol Code

ACVA includes protocol-ish code:

- SSE parsing;
- JSON/YAML parsing;
- HTTP request/response handling;
- prompt construction;
- sentence splitting;
- future transcript streaming protocols.

Rust is a strong fit for these because parsing bugs generally fail safely rather than becoming memory corruption.

### 6. Long-Term Maintainability for a Growing Codebase

If ACVA grows much larger, Rust could reduce maintenance burden around lifetimes, state ownership, and concurrency. It would be particularly helpful if the project grows beyond one developer or starts accepting broad external contributions.

---

## What Rust Would Not Automatically Improve

### 1. Realtime Audio Discipline

Rust does not remove the need for realtime discipline. The audio callback still must not block, allocate, log, lock unpredictably, call slow code, or cross complicated service boundaries.

Rust can help encode some invariants, but the core rules remain design rules:

- no allocation in callback;
- no blocking in callback;
- no I/O in callback;
- bounded handoff only;
- predictable buffer ownership;
- careful sample-clock alignment.

A Rust rewrite would still need highly disciplined audio architecture.

### 2. Native Library Complexity

ACVA depends, or plans to depend, on native systems libraries:

- PortAudio;
- soxr;
- SQLite;
- libcurl or equivalent HTTP/SSE support;
- ONNX Runtime;
- WebRTC Audio Processing Module;
- potentially systemd/sd-bus;
- model runtimes such as llama.cpp and whisper.cpp, though currently out-of-process.

Rust can call these libraries, but many integrations would go through C FFI wrappers. FFI is an unsafe boundary. A Rust rewrite does not eliminate native integration complexity; it moves some of it into `unsafe` wrappers and crate selection.

### 3. System Architecture

The hard parts of ACVA are architectural:

- barge-in correctness;
- AEC loopback alignment;
- cancellation propagation;
- stable turn lifecycle persistence;
- bounded queues and backpressure;
- long-session latency stability;
- service supervision;
- multilingual STT/TTS/LLM coordination;
- recovery after crashes.

Rust would not solve these by itself. These are state-machine and systems-design problems. The current C++ design already addresses them directly.

### 4. Backend Runtime Risk

The project already isolates llama.cpp, whisper.cpp, and Piper as separate processes. This reduces the benefit of rewriting the orchestrator in Rust for crash containment, because the model runtimes are not embedded into the orchestrator address space.

Rust would make the orchestrator safer, but it would not make external model services inherently more reliable.

---

## Costs of a Complete Rust Rewrite

### 1. Roadmap Reset

The biggest cost is schedule and focus. A complete rewrite means rebuilding the same functionality already implemented or planned in C++:

- configuration system;
- logging;
- metrics;
- HTTP control server;
- event bus;
- bounded queues;
- dialogue FSM;
- sentence splitter;
- LLM client;
- SSE parser;
- memory database layer;
- recovery logic;
- TTS client;
- playback queue;
- audio capture;
- resampling;
- VAD;
- supervisor;
- tests.

This would delay the real project goal: a working local voice assistant with reliable long-session behavior and barge-in.

### 2. Loss of Existing Tests and Implementation Knowledge

The repository already has C++ unit tests for many modules. Rewriting in Rust means either porting tests or accepting a temporary drop in test coverage.

Even when ported, the rewrite would introduce new implementation bugs. The project would spend time rediscovering issues already solved in the C++ code.

### 3. FFI and Crate Maturity Risk

Some dependencies have excellent Rust support. Others are more uncertain or are ultimately wrappers around C/C++ APIs.

High-risk areas for Rust dependency maturity include:

- WebRTC APM integration;
- ONNX Runtime packaging across Linux distributions;
- PortAudio callback behavior and realtime constraints;
- soxr binding quality;
- systemd/sd-bus feature completeness;
- compatibility with exact deployment targets.

In C++, these libraries are closer to their native form and fit naturally into CMake-based Linux packaging.

### 4. Build and Packaging Disruption

The current project uses CMake, CMake presets, Compose, systemd units, and native Linux package dependencies.

A complete Rust rewrite would introduce Cargo as the primary build system. That is not bad by itself, but it changes:

- dependency resolution;
- vendoring policy;
- packaging artifacts;
- systemd install assumptions;
- build documentation;
- test commands;
- developer setup;
- possible distro packaging strategy.

This is manageable for a new project, but expensive for an existing C++ project.

### 5. Unsafe Rust Would Still Exist

Because ACVA is systems software with native audio and inference dependencies, a Rust rewrite would not be 100 percent safe Rust.

Likely unsafe zones:

- audio callback FFI;
- raw audio buffer conversion;
- bindings to C libraries;
- possible lock-free ring implementation;
- ONNX Runtime calls;
- WebRTC APM bindings;
- system APIs.

Rust would reduce the unsafe surface, but it would not remove it.

### 6. Risk of Architecture Churn

The current architecture is coherent: C++ orchestrator, external model services, CMake build, local HTTP/SSE, Docker Compose in dev, systemd in production.

A rewrite might tempt the project to also reconsider unrelated decisions:

- Tokio instead of Boost.Asio;
- Rust HTTP server instead of cpp-httplib;
- Rust tracing instead of current logging/metrics plan;
- different SQLite layer;
- different service model;
- different audio abstractions.

That would turn a language rewrite into an architecture rewrite. The project does not need that right now.

---

## Pros of Rewriting Completely in Rust

If the project were rewritten entirely in Rust, the main advantages would be:

1. Stronger memory safety across most of the orchestrator.
2. Stronger compile-time protection against data races.
3. More explicit ownership in async and cross-thread code.
4. Better safety for parser, protocol, and service-client code.
5. Excellent async ecosystem for control-plane and backend-client work.
6. Strong serialization ecosystem through `serde`.
7. Strong structured logging and tracing ecosystem.
8. Potentially easier long-term maintenance if the codebase grows substantially.
9. Better contributor confidence when refactoring complex state ownership.
10. Reduced reliance on manual C++ lifetime discipline.

These are significant. If ACVA were starting from zero today and all required native dependencies had mature Rust bindings, Rust would deserve serious consideration as the primary language.

---

## Cons of Rewriting Completely in Rust

The disadvantages are more decisive for this repository:

1. It discards an existing C++ implementation and test suite.
2. It delays MVP progress substantially.
3. It forces every module to be redesigned or ported.
4. It adds Cargo and Rust dependency management to a currently CMake-native project.
5. It introduces FFI risk for native audio, AEC, VAD, and system libraries.
6. It may require unsafe Rust in exactly the low-level places where correctness is hardest.
7. It creates uncertainty around WebRTC APM and ONNX Runtime integration.
8. It weakens direct alignment with llama.cpp and whisper.cpp ecosystem conventions, even though those are mostly process-isolated.
9. It creates a temporary reliability regression while the new implementation catches up.
10. It shifts attention away from the hardest product risks: AEC, barge-in, STT streaming, cancellation races, and long-session soak stability.
11. It invalidates or complicates current documentation, build instructions, and deployment assumptions.
12. It risks turning a focused language rewrite into a broader architecture rewrite.

For this project, those costs outweigh Rust's benefits.

---

## Component-by-Component Rewrite Assessment

| Component Area | Rewrite in Rust? | Reason |
|---|---:|---|
| Config parsing | Plausible | Rust with `serde` is excellent, but C++ config already exists and is not the main risk |
| HTTP control server | Plausible | Rust `axum`/`hyper` would be strong, but replacing it does not justify full rewrite |
| LLM client and SSE parser | Plausible | Rust is good for streaming parsers, but current C++ libcurl approach is appropriate |
| Dialogue FSM | Possible | Rust enums/state modeling are attractive, but C++ implementation and tests already exist |
| Event bus and queues | Possible | Rust ownership helps, but bounded queue behavior still needs careful design |
| Memory/SQLite layer | Plausible | Rust has good SQLite options, but current C++ layer matches existing design |
| Logging/metrics/tracing | Plausible | Rust ecosystem is strong, but current C++ stack is serviceable |
| Playback queue | Possible | Rust helps with ownership, but realtime/audio integration still dominates |
| Audio callback and capture | Risky | Rust can do it, but FFI and realtime constraints reduce the benefit |
| Resampling with soxr | Risky | Binding maturity and unsafe audio buffer handling matter |
| VAD with ONNX Runtime | Risky | Rust wrappers may work, but native integration and deployment become uncertain |
| WebRTC APM / AEC | High risk | C++ integration is likely more direct; Rust may need complex bindings |
| Supervisor/systemd | Plausible | Rust can integrate with systemd, but current MVP dev path mostly uses HTTP probes |
| Tests | Plausible | Rust testing is good, but all current tests would need to be ported |

The table shows that many individual modules could be written well in Rust. But the complete system rewrite is not attractive because the highest-risk modules are exactly the low-level native integration points.

---

## Strategic Recommendation

### Do Not Rewrite the Project Completely in Rust

The C++ version should continue as the MVP path.

Reasons:

- the project is already architected around C++23;
- the implementation and tests already exist in C++;
- the native dependency stack fits C++ well;
- the model runtimes are already isolated by process boundaries;
- the remaining project risks are architectural and product-level, not solved merely by changing languages;
- a rewrite would delay delivery more than it would reduce risk.

### Harden the C++ Implementation Instead

The safer path is to improve C++ reliability:

1. Add sanitizer presets for AddressSanitizer, UndefinedBehaviorSanitizer, and ThreadSanitizer where practical.
2. Keep warnings strict.
3. Expand cancellation-race tests.
4. Expand queue overflow/backpressure tests.
5. Add long-session soak tests early.
6. Document realtime callback contracts at every audio API boundary.
7. Minimize custom lock-free structures beyond the SPSC ring.
8. Keep model runtimes out-of-process through MVP.
9. Prefer simple ownership patterns and avoid clever shared-state designs.

### Consider Rust Later, Selectively

Rust should remain an option for future standalone components, especially:

- log and trace analysis tools;
- conversation export or admin CLI tools;
- a standalone streaming STT gateway;
- a future web/control API service;
- protocol adapters or parsers behind process boundaries.

The key rule should be:

> Rust is acceptable behind a process, CLI, file, or network boundary. Avoid in-process C++/Rust mixing unless there is a measured, specific benefit.

---

## Decision Matrix

| Option | Benefits | Costs | Recommendation |
|---|---|---|---|
| Continue C++23 MVP | Preserves current work, aligns with docs, best native dependency fit, fastest path to working assistant | Requires C++ discipline around memory and concurrency | Recommended |
| Full Rust rewrite now | Best memory-safety story, strong async ecosystem, safer refactors | Large delay, rewrite risk, FFI risk, packaging disruption, loss of current momentum | Not recommended |
| Hybrid C++ core + Rust modules | Allows Rust where useful | FFI complexity, split tooling, ownership-boundary bugs | Avoid for MVP |
| Rust standalone tools/services later | Gains Rust benefits without destabilizing core | Adds another language only where bounded | Good post-MVP option |

---

## Final Verdict

Rust is a strong language for this type of software, and a greenfield Rust ACVA would be a defensible design. But this repository is not greenfield anymore, and its architecture is already deliberately structured to reduce the main risks of C++ by isolating model runtimes, enforcing bounded queues, making cancellation explicit, and keeping realtime audio paths narrow.

A complete Rust rewrite would be strategically wrong right now. It would trade known C++ engineering work for a large rewrite whose benefits are real but not urgent enough to justify the cost.

The project should stay in C++23 for MVP, harden the C++ implementation aggressively, and reserve Rust for isolated post-MVP services or tooling where it can provide value without destabilizing the core orchestrator.
