# M1 — LLM + Memory

**Estimate:** 1–2 weeks.

**Depends on:** M0 (event bus, FSM, config, logging).

**Blocks:** M2 (supervisor probes the LLM service), M3 (TTS bridge consumes `LlmSentence`), M5 (streaming partial STT replaces the simple final-only flow).

## Goal

Drive a real LLM end-to-end against a console transcript: text in → tokens streamed → sentences split → spoken sentences logged → turn persisted. No audio yet. By the end of M1, you can have a text-only conversation with the orchestrator using llama.cpp + Qwen2.5-7B; memory accumulates across turns; the orchestrator restarts cleanly mid-conversation.

## Out of scope

- Real audio (M4) — sentences emit as `LlmSentence` events and get logged, not spoken.
- Real summarization quality tuning. M1 lands the *machinery* (async memory thread, summary table, source-hash). The summarizer prompt itself is a stub that records "[TODO summary]" until we evaluate prompt variants.
- TTS / playback (M3).
- Service supervision (M2). M1 assumes llama.cpp is up; the supervisor watches it in M2.

## New deps

| Lib | Version | Purpose | Source |
|---|---|---|---|
| libcurl (dev) | 7.80+ | LLM SSE streaming | system pkg |
| sqlite3 (dev) | 3.42+ | memory storage | system pkg |
| (no client lib) | — | SQLite accessed via the C API directly. We'll write a thin wrapper rather than pulling SQLiteCpp; volume is small. |

Add to `cmake/Dependencies.cmake`:
```cmake
find_package(CURL REQUIRED)
find_package(SQLite3 REQUIRED)
```

## Step 1 — Memory thread + SQLite wrapper

**Files:**
- `src/memory/db.hpp` — RAII `Database` wrapper over `sqlite3*`, prepared-statement cache, WAL/synchronous=NORMAL pragmas applied on open.
- `src/memory/db.cpp`
- `src/memory/schema.sql` — embedded via `cmake_path` + `configure_file` or a generated header (probably a generated `schema_sql.hpp` via `file(READ ...)` at configure time).
- `src/memory/repository.hpp` — typed read/write methods (`insert_turn`, `update_turn_status`, `latest_summary`, `recent_turns(N)`, `upsert_fact`, ...).
- `src/memory/repository.cpp`
- `src/memory/memory_thread.hpp` — owns the DB; serializes writes from a single dedicated thread; reads via `MemoryThread::read([](auto& repo){...})` posting onto its strand.
- `src/memory/memory_thread.cpp`

**Schema:** identical to project_design.md §9.1. Embed it as a string and run on first open.

**API sketch:**
```cpp
class MemoryThread {
public:
    MemoryThread(std::filesystem::path db_path);
    ~MemoryThread();

    // Fire-and-forget write; returns a future for callers that want to wait.
    std::future<void> write(std::function<void(Repository&)> op);

    // Synchronous read. Internally posts to the memory thread and waits.
    template <class R>
    R read(std::function<R(Repository&)> op);
};
```

**Acceptance:** `tests/test_memory.cpp` covers schema creation, turn insert/read, recovery sweep (next step).

## Step 2 — Crash-recovery sweep

**Files:** `src/memory/recovery.hpp`, `src/memory/recovery.cpp`.

**Behavior on `Database::open()`:**
1. For each session with `ended_at IS NULL`: set `ended_at = COALESCE(MAX(turns.ended_at), started_at)`.
2. For each turn with `status = 'in_progress'`: set `status = 'interrupted'`, `interrupted_at_sentence = NULL`.
3. For each summary, recompute `source_hash` over the source turn texts; if it diverges, mark a stale-summary log line (regenerated lazily later by the summarizer).

Recovery happens in a single transaction. Logged at info: `recovery: closed N sessions, marked M turns interrupted, K stale summaries`.

**Tests:** `tests/test_recovery.cpp` — open a DB pre-populated with crashed states, recovery returns expected counts, queries return consistent state.

## Step 3 — `lang` column wiring

The schema gained `lang` columns in the design; M1 fills them. Most-used `lang` for assistant turns in M1 is "en" since multilingual STT is M5.

## Step 4 — Prompt builder

**Files:** `src/llm/prompt_builder.hpp`, `src/llm/prompt_builder.cpp`.

**Structure** (project_design.md §9.3):
```
[system policy]              ← from cfg.dialogue.system_prompts[lang]
[user preferences]           ← `settings` table (M1 just reads, doesn't yet write)
[durable facts]              ← `facts` table where confidence > threshold
[cached session summary]     ← latest `summaries` row
[last N turns verbatim]      ← N from cfg.dialogue.recent_turns_n (default 10)
[current user turn]
```

Output: an OpenAI-compatible `messages` JSON payload (system + alternating user/assistant + final user).

**API:**
```cpp
struct PromptInputs {
    std::string lang;                       // detected language (passed to system prompt)
    std::string current_user_text;
};

class PromptBuilder {
public:
    PromptBuilder(const Config& cfg, MemoryThread& memory);
    std::string build(const PromptInputs& in); // returns serialized messages JSON
    [[nodiscard]] std::size_t last_token_estimate() const noexcept; // cheap estimate
};
```

**Token-count estimate**: rough — 1 token per ~4 chars for budget enforcement. We'll wire a real tokenizer in M1's tail or M3 if the estimate proves too loose.

**Tests:** snapshot tests with fixed memory contents → prompt JSON is stable byte-for-byte.

## Step 5 — Sentence splitter

**Files:** `src/dialogue/sentence_splitter.hpp`, `src/dialogue/sentence_splitter.cpp`.

**Spec** (project_design.md §4.8): streaming token-in, sentence-out. Emit when:
- Terminal punctuation (`.!?`) seen, AND
- followed by whitespace+capital, OR end-of-stream, OR tokens-since-punctuation exceeds `max_sentence_tokens` (forced flush).

Must handle:
- Abbreviations: `Dr.`, `Mr.`, `Mrs.`, `Ms.`, `e.g.`, `i.e.`, `etc.`, `vs.`, common short titles. Configurable allowlist.
- Decimals: `3.14`, `0.5`, version `v1.2.3`.
- Enumerations: `1.`, `2.`, `a)`. These reset the sentence boundary detector.
- Code fences (` ``` `): suppress splitting inside fenced blocks; emit the entire block as a single "sentence" so TTS can do the right thing later (or skip it).
- Ellipses (`...`, `…`): not a sentence boundary.
- Multilingual: if `lang != "en"`, the punctuation set extends (e.g., `。` for ja, `！` for zh). Skip for M1; English is enough for M1.

**API:**
```cpp
class SentenceSplitter {
public:
    explicit SentenceSplitter(const SentenceSplitterConfig& cfg);

    // Feed an incremental token. May produce zero or more complete sentences.
    void push(std::string_view token, std::string_view lang,
              std::vector<std::string>& out_sentences);

    // Stream end — flushes the buffered tail as a final sentence if non-empty.
    void flush(std::vector<std::string>& out_sentences);

    void reset(); // call between turns
};
```

**Tests** — `tests/test_sentence_splitter.cpp`. Golden corpus including the cases above plus pathological inputs:
- `"Dr. Smith said hello. What now?"` → 2 sentences.
- `"The price is $3.14. Or maybe $4.20?"` → 2 sentences.
- `"1. First item.\n2. Second item.\n3. Third."` → 3 sentences.
- `"Loading..."` → 1 sentence.
- A 4 KB chunk of text without any punctuation → emits at `max_sentence_tokens` boundary.

## Step 6 — LLM client

**Files:** `src/llm/client.hpp`, `src/llm/client.cpp`.

Two HTTP libraries on purpose (per the locked design):
- Non-streaming requests (health probe, prompt token count) → cpp-httplib.
- SSE streaming completion → libcurl with the `CURLOPT_WRITEFUNCTION` callback.

**Threading:** the LLM client owns one I/O thread that runs the libcurl easy handle. Multiple in-flight requests are serial in M1 (one turn at a time anyway). Reentrant in M5 when speculation arrives.

**API:**
```cpp
struct LlmRequest {
    std::string messages_json;     // pre-built by PromptBuilder
    int max_tokens = 400;
    float temperature = 0.7F;
    std::shared_ptr<dialogue::CancellationToken> cancel;
    dialogue::TurnId turn;         // tagged onto every emitted event
    std::string lang;
};

struct LlmCallbacks {
    std::function<void(std::string token)> on_token;     // streamed
    std::function<void(LlmFinish)> on_finished;          // tokens_total, cancelled, error
};

class LlmClient {
public:
    LlmClient(const Config& cfg, event::EventBus& bus);
    ~LlmClient();

    // Non-blocking. Posts the request to the I/O thread; returns when the
    // request has been queued. Cancellation via the token in the request.
    void submit(LlmRequest req, LlmCallbacks cb);

    // Health probe. Returns true on success; logs detail on failure.
    bool probe();

    // Keep-alive completion: 1 max_token request to prevent model offload.
    // The supervisor (M2) drives this on a timer; M1 just exposes the API.
    void keep_alive();
};
```

**Implementation notes:**
- The request is a POST to `/v1/chat/completions` with `stream: true`.
- SSE parser handles `data: {...}\n\n` chunks, the `[DONE]` sentinel, and OpenAI-compatible delta format (`choices[0].delta.content`).
- Cancellation is checked between SSE chunks — if `req.cancel->is_cancelled()`, libcurl is asked to abort (`return 0` from the write callback).

**Tests:**
- `tests/test_llm_client.cpp` — fakes via cpp-httplib's `Server` (in-process test fixture). One golden test that streams 5 fake SSE chunks, asserts on_token is called 5x, on_finished receives correct count.
- Real llama.cpp smoke test: gated behind a `LCLVA_REAL_LLM=1` env var so it runs only when configured. Spins a tiny model (Qwen2.5-0.5B) and asks one question.

## Step 7 — Dialogue Manager wiring

The FSM in M0 is reactive: events go in, transitions come out. M1 adds the active behaviour:

When the FSM enters `Thinking`, the Dialogue Manager:
1. Reads `FinalTranscript` from the event.
2. Calls `PromptBuilder::build()` synchronously (memory reads are fast — O(ms)).
3. Submits to `LlmClient` with the active turn's `CancellationToken`.
4. Wires `on_token` → `SentenceSplitter::push` → publish `LlmSentence` events for each complete sentence.
5. On `on_finished`: publish `LlmFinished`. If cancelled, the FSM's existing handler does the right thing (UserInterrupted path bumped the turn already).

In M0, the fake driver simulates these. M1 replaces it with the real glue.

**Files:**
- `src/dialogue/manager.hpp`, `src/dialogue/manager.cpp` — the active glue. Subscribes to `FinalTranscript`; orchestrates prompt build → LLM → sentence stream.
- Update `src/dialogue/fsm.hpp` — add a `Manager` reference, or have the Manager subscribe to bus events independently and the FSM only reacts to the resulting events. Prefer the second: keeps the FSM pure.

**Recommendation:** Manager is its own component, separate from the FSM. The FSM remains a pure state machine driven by events. The Manager reads `FinalTranscript`, runs the LLM, emits `LlmSentence` and `LlmFinished` events. The FSM transitions on those events as it already does in M0.

## Step 8 — Turn persistence + outcome wiring

When the FSM observes a turn outcome:
- `completed`: write a `turns` row with `role='assistant'`, the concatenated sentence text, `lang`, `status='committed'`.
- `interrupted`: same, but `status='interrupted'` and the `text` includes only the sentences that finished playback (M1 has no playback, so it's "all sentences before UserInterrupted arrived").
- `discarded`: do not persist. Log a counter increment.

Also write the user turn (status='committed') when `FinalTranscript` is observed.

**Files:** `src/dialogue/turn_writer.hpp`, `src/dialogue/turn_writer.cpp` — a small subscriber that listens to FSM outcomes (via a callback on the FSM, mirror of the metrics observer) and posts writes to the memory thread.

## Step 9 — Async summarization stub

**Files:** `src/memory/summarizer.hpp`, `src/memory/summarizer.cpp`.

For M1 the summarizer:
- Triggers on `cfg.memory.summary.turn_threshold` new turns since last summary (configurable).
- Runs on the memory thread (must not block dialogue).
- Calls `LlmClient::submit` with a compact prompt that asks for a 200-word summary of the source range.
- Writes a `summaries` row with the result and the `source_hash`.

**Tunable:** `cfg.memory.summary.{trigger, turn_threshold, token_threshold, idle_seconds}`.

The actual quality of the summarization prompt is *not* an M1 deliverable. Prompt iteration happens in M8 against soak-test recordings.

## Step 10 — JSON structured logging

Replace M0's structured-text logging with a custom spdlog formatter or a custom sink that emits JSON:
```json
{"ts":"2026-04-30T12:34:56.789+04:00","level":"info","component":"dialogue","event":"llm_first_token","turn_id":42,"latency_ms":381}
```

**Files:** new `src/log/json_sink.hpp`, `src/log/json_sink.cpp`. Implements `spdlog::sinks::base_sink<std::mutex>::sink_it_` and serializes per-line.

The `lclva::log::info(component, message)` API stays. Adds:
```cpp
void event(std::string_view component, std::string_view event_name,
           dialogue::TurnId turn,
           std::initializer_list<std::pair<std::string_view, std::string>> kv);
```

For machine-friendly tracing.

## Step 11 — Configuration extension

Add to `Config`:
```yaml
llm:
  base_url: "http://127.0.0.1:8081/v1"
  model: "qwen2.5-7b-instruct"
  temperature: 0.7
  max_tokens: 400
  max_prompt_tokens: 3000
  request_timeout_seconds: 60
  keep_alive_interval_seconds: 60

memory:
  db_path: "${XDG_DATA_HOME:-~/.local/share}/lclva/lclva.db"
  recent_turns_n: 10
  summary:
    trigger: turns           # turns | tokens | idle | hybrid
    turn_threshold: 15
    token_threshold: 4000
    idle_seconds: 120
    language: dominant       # dominant | english | recent
  facts:
    policy: conservative     # conservative | moderate | aggressive | manual_only
    confidence_threshold: 0.7

dialogue:
  recent_turns_n: 10
  max_assistant_sentences: 6
  max_assistant_tokens: 400
  system_prompts:
    en: |
      You are a local voice assistant.
      Answer in short spoken paragraphs.
      Prefer direct answers over long lists.
      Ask at most one clarification question.
      For technical topics, give concise but precise explanations.
```

## Test plan

| Test | Scope |
|---|---|
| `test_db.cpp` | open / pragmas / round-trip / WAL behavior |
| `test_recovery.cpp` | crash sweep correctness |
| `test_repository.cpp` | typed read/write methods |
| `test_prompt_builder.cpp` | snapshot tests across memory states |
| `test_sentence_splitter.cpp` | golden corpus (cases above) |
| `test_llm_client.cpp` | fake server fixture; SSE parsing; cancellation; timeout |
| `test_manager.cpp` | end-to-end with fake LLM client driving the Dialogue Manager |
| Real-LLM smoke | `LCLVA_REAL_LLM=1` against a tiny model |

## Acceptance

1. With llama.cpp + Qwen2.5-7B running locally, `--config config/m1.yaml` accepts text input on stdin, streams an answer (logged as `LlmSentence` events), and writes both turns to SQLite with `status='committed'`.
2. Killing the orchestrator mid-turn and restarting it: the recovery sweep marks the in-flight turn `interrupted`, the next session opens cleanly, prior session's `ended_at` is set.
3. `tests/test_sentence_splitter.cpp` golden corpus passes.
4. SSE cancellation: setting the turn's cancellation token mid-stream causes the LLM client to abort and emit `LlmFinished{ cancelled=true }` within 100 ms.
5. Logs are valid JSON, one event per line; per-turn `turn_id` field is populated for every event after the FSM mints a turn.
6. `voice_llm_first_token_ms` and `voice_llm_tokens_per_sec` metrics emit non-zero values.

## Risks specific to M1

| Risk | Mitigation |
|---|---|
| SSE parser misses or duplicates events | Use libcurl, golden test fixture with SSE byte sequences captured from llama.cpp |
| Prompt assembly slow at deep context | Token-estimate cap in `PromptBuilder::build`; trigger summarization on overflow |
| Memory thread saturates under bursty writes | Bounded write queue (default 256); drop+log oldest with metric on overflow; soak test |
| SQLite schema migrations later | M1 establishes v1 schema; add `pragma user_version = 1` and bake a migration framework before extending in M3 |
| JSON logger performance | spdlog's async logger; the JSON formatter must not allocate per-call beyond a small reusable buffer |

## Time breakdown

| Step | Estimate |
|---|---|
| 1–3 Memory + recovery | 2 days |
| 4 Prompt builder | 1 day |
| 5 SentenceSplitter | 2 days (golden corpus is half the work) |
| 6 LLM client | 2 days |
| 7 Dialogue Manager wiring | 1 day |
| 8 Turn persistence | 0.5 day |
| 9 Summarizer stub | 0.5 day |
| 10 JSON logging | 1 day |
| 11 Config extension | 0.5 day |
| Tests + soak | 1.5 days |
| **Total** | **~12 days** = ~2.5 weeks (vs the 1–2 week estimate; the extra is the SentenceSplitter golden corpus and JSON logger) |
