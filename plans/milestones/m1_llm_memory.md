# M1 — LLM + Memory

**Estimate:** 2–3 weeks across three slices.

**Depends on:** M0 (event bus, FSM, config, logging).

**Blocks:** M3 (TTS bridge consumes `LlmSentence`), M5 (streaming partial STT replaces the simple final-only flow).

## Goal

Drive a real LLM end-to-end against a console transcript: text in → tokens streamed → sentences split → spoken sentences logged → turn persisted. No audio yet. By the end of M1, you can have a text-only conversation with the orchestrator using llama.cpp + Qwen2.5-7B; memory accumulates across turns; the orchestrator restarts cleanly mid-conversation.

## Out of scope

- Real audio (M4) — sentences emit as `LlmSentence` events and get logged, not spoken.
- TTS / playback (M3).
- Service supervision proper (M2 — now retargeted to HTTP health probes; see m2_supervision.md).
- Production-quality summarization prompt tuning. M1 lands the machinery; the summarizer's prompt is a stub recording `"[TODO summary]"` until we evaluate prompt variants in M8.

## Deployment shape

For dev (M1 onward), the **model backends run as Docker Compose containers on the host network**; **`lclva` runs on the host as a CLI** so debugging / step-through / log inspection is direct. The systemd unit files in `packaging/systemd/` remain valid for production-style deployment but are no longer the default path.

This decision is captured in `plans/open_questions.md` as a follow-up to H4/H5; the design doc's §4.12 (Supervisor) and §16 (tech stack) are revised accordingly.

---

# Part A — Complete (Slice 1, landed)

What's in the tree as of this writing.

## A.0 New deps wired in CMake

| Lib | Used in | `find_package` |
|---|---|---|
| SQLite3 | memory layer | `find_package(SQLite3 REQUIRED)` → `SQLite::SQLite3` |
| libcurl (dev) | reserved for LLM SSE (slice 2) | `find_package(CURL REQUIRED)` → `CURL::libcurl` |

Both linked through `lclva_core` so tests pick them up.

## A.1 Configuration extension ✅

`src/config/config.{hpp,cpp}` extended with:

- `LlmConfig` — base_url, model, unit, temperature, max_tokens, max_prompt_tokens, request_timeout_seconds, keep_alive_interval_seconds.
- `MemoryConfig` — db_path, recent_turns_n, write_queue_capacity, nested SummaryConfig + FactsConfig.
- `DialogueConfig` — recent_turns_n, max_assistant_sentences, max_assistant_tokens, fallback_language, system_prompts (`std::map<std::string, std::string>`), nested SentenceSplitterConfig.
- Validation: temperature in [0,2], max_tokens > 0, summary.trigger ∈ {turns, tokens, idle, hybrid}, facts.policy ∈ {conservative, moderate, aggressive, manual_only}, system_prompts (when non-empty) must contain the fallback_language.

`config/default.yaml` extended in lockstep.

## A.2 SQLite database wrapper ✅

`src/memory/db.{hpp,cpp}` — RAII wrappers:

- `Database` — opens with WAL + `synchronous=NORMAL` + `foreign_keys=ON`; nested `Transaction` guard with commit/rollback/auto-rollback-on-destruct.
- `Statement` — prepared statement with bind helpers (incl. `optional<...>` overloads); step returns Row/Done/Error; column accessors.
- `Result<T>` = `std::variant<T, DbError>` so we don't depend on `std::expected`.

## A.3 Schema + repository ✅

`src/memory/schema.hpp` — embedded SQL string. Tables: sessions, turns (with `lang`), summaries (with `lang` + `source_hash`), facts (UNIQUE(key, lang)), settings. Indexes on (session_id, id), (session_id, range_end_turn), and (key). `pragma user_version = 1`.

`src/memory/repository.{hpp,cpp}` — typed CRUD. Notable methods:

- `insert_session`, `close_session`, `sessions_open_no_ended_at`
- `insert_turn`, `set_turn_status`, `recent_turns(session, limit)`, `turns_in_progress`, `max_turn_ended_at`
- `insert_summary`, `latest_summary`, `all_summaries`
- `upsert_fact` (ON CONFLICT(key, lang) DO UPDATE)
- `set_setting`, `get_setting`
- `database()` accessor — used by `run_recovery` to wrap its sweep in a `Transaction`.

## A.4 Memory thread ✅

`src/memory/memory_thread.{hpp,cpp}`:

- Single dedicated writer/reader thread holding the only `Database`.
- Bounded job queue (`event::BoundedQueue<WriteJob>`, default DropNewest, capacity from config).
- API: `submit<Fn>(fn)` returns `std::future<R>`; `read<Fn>(fn)` blocking helper; `post(job)` fire-and-forget.
- Posting order is preserved → causal consistency.
- Drops counter exposed for metrics.

## A.5 Recovery sweep ✅

`src/memory/recovery.{hpp,cpp}`:

- `run_recovery(repo, db)` runs in a single transaction:
  1. For every session with `ended_at IS NULL`: set `ended_at = MAX(turns.ended_at)` (or session's `started_at` if none).
  2. For every `status='in_progress'` turn: set to `interrupted`.
  3. For every summary: recompute the FNV-1a-64 source-hash and count stale ones.
- Returns counters; idempotent.
- Wired into `main.cpp`: runs synchronously at startup, logs the summary, fails the orchestrator if the sweep errors.

## A.6 Sentence splitter ✅

`src/dialogue/sentence_splitter.{hpp,cpp}` — streaming token-in / sentence-out. Implementation notes worth recording:

- **Pending-terminator state machine.** A terminator (`.!?`) doesn't immediately split — it goes pending and resolves on the next char. This handles decimals (`3.14`) and ellipsis runs uniformly.
- **Dot-run counter** (`pending_dot_run_`): when ≥ 2, the run is treated as ellipsis and the buffer keeps growing (no split). Catches `Loading...` and the Unicode `…`.
- **List-marker check before decimal.** `1. First item` would fail decimal check (digit-before-dot) but list-marker is more specific (short word + line start), so list-marker wins.
- **Code-fence suppression** when `cfg.detect_code_fences = true`.
- **Forced flush** at `max_sentence_chars` on the next whitespace-or-comma. Prevents TTS starvation on long unpunctuated chunks.

48 tests total across the project, 24 of which are M1 slice-1: db, repository, recovery, memory_thread, sentence_splitter.

## A.7 Wired into main.cpp ✅

Startup sequence:

1. Load config (existing).
2. Init logging (existing).
3. **Open MemoryThread** (M1.A) — creates DB if missing, applies pragmas, runs schema.
4. **Run recovery sweep** synchronously (M1.A) — log summary line.
5. Build event bus + metrics + FSM (existing M0).
6. Build HTTP control plane (existing M0).
7. Optional fake driver (existing M0).
8. Block on signal; orderly shutdown.

The M0 fake driver still runs; sentences travel as events but aren't yet routed through the LLM/SentenceSplitter. That happens in slice 2.

---

# Part B — Environment setup (Docker Compose, upstream images) ✅ landed

What you do **before** starting slice 2. The output is a `docker-compose.yml` that brings up llama.cpp / whisper.cpp / piper, all reachable on `127.0.0.1` ports that match the lclva config.

**Update from earlier draft:** all three backends ship official-or-upstream HTTP servers. We do *not* write or build custom Dockerfiles in M1. We use upstream images verbatim. The custom whisper-streaming wrapper is M5's concern (and may itself be skipped — see m5_streaming_stt.md).

**Status:** `packaging/compose/docker-compose.yml`, root-level `compose.yaml` shim, and root-level `.env.example` are in the tree as of this writing. Four additions vs. the draft below worth recording:

- A two-line `compose.yaml` at the project root re-includes `packaging/compose/docker-compose.yml` so `docker compose up` works from the repo root without `-f`. Definitions stay under `packaging/compose/` per the layout principle; the root shim is just discovery. `.env.example` lives at the project root alongside it (Compose's default `.env` lookup dir), and `.env` is gitignored.

- The compose `command` for each service is wired to the env-override variables, not just the host paths. `LCLVA_LLM_MODEL`, `LCLVA_WHISPER_MODEL`, and `LCLVA_PIPER_VOICE` all flow through. The plan listed only `LCLVA_LLM_MODEL` in `.env.example` but didn't actually expand it inside `command:` — which would have made the override a dead variable.
- `--alias qwen2.5-7b-instruct` is added to the llama command so the OpenAI-compatible endpoint reports the same model name that `llm.model` carries in `config/default.yaml`. Saves a moment of confusion when staring at request/response bodies.
- `start_period:` is set per service (30 s llama / 15 s whisper / 60 s piper) because piper does a `pip install` on first boot and the default 0-second grace would mark it `unhealthy` before the install finishes.

End-to-end smoke (B.7 acceptance) still has to be run on the dev machine — it requires NVIDIA Container Toolkit, ~5 GB of model files, and image pulls that aren't worth doing in a sandboxed automation context. `docker compose config --quiet` validates the YAML locally and was clean.

## B.1 Prerequisites

- Docker Engine ≥ 26 (Compose v2 built-in). Podman ≥ 4 with `podman-compose` works as well.
- For GPU: **NVIDIA Container Toolkit** ≥ 1.14.
  - Arch: `sudo pacman -S nvidia-container-toolkit && sudo systemctl restart docker`
  - Debian/Ubuntu: follow upstream NVIDIA Container Toolkit install guide.
  - Verify: `docker run --rm --gpus all nvidia/cuda:12.6.0-base-ubuntu22.04 nvidia-smi` prints the GPU table.
- User must be in the `docker` group (or use rootless Docker).
- Model files already downloaded on the host under `~/.local/share/lclva/models/` and voices under `~/.local/share/lclva/voices/` (see README §"Setting up the runtime services").

## B.2 File layout

```
compose.yaml                 # top-level shim — `include:`s the file below
.env.example                 # paths to model files, port overrides
packaging/compose/
  docker-compose.yml         # service definitions
  whisper/Dockerfile         # local whisper-server build (no upstream image exists)
  piper/Dockerfile           # local piper-tts image (no per-boot pip install)
```

llama.cpp uses the upstream `ghcr.io/ggml-org/llama.cpp:server-cuda` image verbatim. Models live on the host and are bind-mounted read-only into each container.

**Original draft said "no Dockerfiles in M1.B".** That guidance was based on the assumption that all three backends had publishable upstream HTTP-server images. Verification at first build showed two of them did not, so this milestone now ships two minimal Dockerfiles. See the Status note above for the full reasoning.

**Why the root-level shim:** Compose v2 auto-discovers `compose.yaml` in the working directory, so `docker compose up` from the project root just works without `-f packaging/compose/docker-compose.yml`. The shim is two non-comment lines and `include:`s the canonical file under `packaging/compose/`. `.env` lives at the project root (where Compose looks by default) and `.env.example` lives next to it; `.env` is gitignored.

## B.3 docker-compose.yml shape

```yaml
name: lclva

services:
  # llama.cpp — official ggml-org image with CUDA build of llama-server.
  llama:
    image: ghcr.io/ggml-org/llama.cpp:server-cuda
    ports:
      - "127.0.0.1:8081:8081"
    volumes:
      - ${LCLVA_MODELS_DIR:-${HOME}/.local/share/lclva/models}:/models:ro
    deploy:
      resources:
        reservations:
          devices:
            - driver: nvidia
              count: 1
              capabilities: [gpu]
    healthcheck:
      test: ["CMD", "curl", "-fsS", "http://127.0.0.1:8081/health"]
      interval: 5s
      timeout: 2s
      retries: 12
    restart: unless-stopped
    command: >
      --host 0.0.0.0 --port 8081
      --model /models/qwen2.5-7b-instruct-q4_k_m.gguf
      --ctx-size 8192 --n-gpu-layers 999 --threads 4 --metrics

  # whisper.cpp — official server image. Sync request/response transcription;
  # M1-M4 only feed full utterances anyway. M5 may swap this for a streaming
  # backend (Speaches / faster-whisper / custom wrapper — see m5).
  whisper:
    image: ghcr.io/ggml-org/whisper.cpp:server
    ports:
      - "127.0.0.1:8082:8082"
    volumes:
      - ${LCLVA_MODELS_DIR:-${HOME}/.local/share/lclva/models}:/models:ro
    healthcheck:
      test: ["CMD", "curl", "-fsS", "http://127.0.0.1:8082/health"]
      interval: 5s
      timeout: 2s
      retries: 12
    restart: unless-stopped
    command: ["--host", "0.0.0.0", "--port", "8082", "--model", "/models/ggml-small.bin"]

  # Piper — runs the upstream piper.http_server Python module. We use the
  # python base image and `pip install piper-tts`; the rhasspy project does
  # not yet publish an official HTTP image. The shim is two lines.
  piper:
    image: python:3.12-slim
    ports:
      - "127.0.0.1:8083:8083"
    volumes:
      - ${LCLVA_VOICES_DIR:-${HOME}/.local/share/lclva/voices}:/voices:ro
    healthcheck:
      test: ["CMD", "python", "-c", "import urllib.request; urllib.request.urlopen('http://127.0.0.1:8083/').read()"]
      interval: 5s
      timeout: 2s
      retries: 12
    restart: unless-stopped
    command: >
      sh -c "pip install --no-cache-dir piper-tts &&
             python -m piper.http_server
             --host 0.0.0.0 --port 8083
             --model /voices/en_US-amy-medium.onnx"
```

**Tag/image notes** (verify at first build; pin to digests once verified):

- `ghcr.io/ggml-org/llama.cpp:server-cuda` — published by the llama.cpp project; CUDA 12.x.
- `ghcr.io/ggml-org/whisper.cpp:server` — published by the whisper.cpp project. CPU build sufficient (B1 decision).
- `python:3.12-slim` — official; the `pip install piper-tts` happens at container start. To pin: build a tiny derived image once that pre-installs `piper-tts`, push to a personal registry, replace the inline `pip install`.

Network mode: default bridge with port mapping to `127.0.0.1`. Avoids the `network_mode: host` complications on rootless Docker / WSL2 while keeping everything on localhost.

If `ghcr.io/ggml-org/whisper.cpp:server` turns out not to exist at the right tag, fall back to building from upstream once and tagging locally:
```sh
git clone https://github.com/ggml-org/whisper.cpp.git && cd whisper.cpp
docker build -t local/whisper.cpp:server -f .devops/server.Dockerfile .
```
Replace `image:` accordingly.

## B.4 .env.example

```ini
# Override model/voice paths (default: ~/.local/share/lclva/...)
# LCLVA_MODELS_DIR=/path/to/models
# LCLVA_VOICES_DIR=/path/to/voices

# Switch the LLM model file without editing docker-compose.yml:
# LCLVA_LLM_MODEL=qwen2.5-7b-instruct-q4_k_m.gguf
```

## B.5 Operating the stack

Bring up:
```sh
cd packaging/compose
docker compose up -d                # first run: ~5 min for image pulls
docker compose ps                    # all 3 healthy after ~30 s once images cached
docker compose logs -f llama         # live log of one service
```

Stop / clean:
```sh
docker compose down                  # stop, keep images and volumes
docker compose down -v --rmi all     # nuke everything
```

Switching the LLM model is a host-side file swap + `docker compose restart llama`.

## B.6 Health checks the orchestrator relies on

`lclva` polls each backend's `/health` endpoint per `cfg.supervisor.probe_interval_*_ms`. Compose's `healthcheck:` is independent — it dictates whether Compose marks the container ready/unhealthy and whether `restart: unless-stopped` kicks in. Both layers exist on purpose: Compose handles process lifecycle; the orchestrator handles application-level state and dialogue gating.

## B.7 Verifying B is done

Acceptance:

1. `docker compose up -d` brings all three services to `healthy` within 60 s of pull/start.
2. `curl -fsS 127.0.0.1:8081/health` returns 200; same for 8082 and 8083.
3. Submitting a manual completion to llama works:
   ```sh
   curl -sS -X POST http://127.0.0.1:8081/v1/chat/completions \
     -H 'Content-Type: application/json' \
     -d '{"model":"qwen2.5-7b-instruct","messages":[{"role":"user","content":"hi"}],"max_tokens":20}'
   ```
4. With the orchestrator running on the host (M0 fake-driver mode), `curl /status` still works — host orchestrator is unaware of containers at this stage.

---

# Part C — Slices 2 & 3 ✅ landed

The components below assume Part B has been completed at least once on the dev machine.

## C.1 Slice 2 — LLM + Dialogue Manager + persistence

### C.1.1 Prompt builder

`src/llm/prompt_builder.{hpp,cpp}`. Structure (project_design.md §9.3):

```
[system policy]              ← cfg.dialogue.system_prompts[lang]
[user preferences]           ← settings table (read-only in M1)
[durable facts]              ← facts table where confidence > threshold
[cached session summary]     ← latest summaries row
[last N turns verbatim]      ← N from cfg.dialogue.recent_turns_n
[current user turn]
```

Output: OpenAI-compatible `messages` JSON (system + alternating user/assistant + final user). Token-count estimate ≈ 1 token per ~4 chars; replaced by a real tokenizer if the estimate proves too loose.

API:
```cpp
struct PromptInputs {
    std::string lang;
    std::string current_user_text;
};

class PromptBuilder {
public:
    PromptBuilder(const Config& cfg, MemoryThread& memory);
    std::string build(const PromptInputs&);
    [[nodiscard]] std::size_t last_token_estimate() const noexcept;
};
```

Tests: snapshot tests with fixed memory state → byte-stable prompt JSON.

### C.1.2 LLM client (libcurl SSE + cpp-httplib for non-streaming)

`src/llm/client.{hpp,cpp}`. One I/O thread runs libcurl easy handles. Reentrant in M5; M1 keeps it serial.

```cpp
struct LlmRequest {
    std::string messages_json;
    int max_tokens = 400;
    float temperature = 0.7F;
    std::shared_ptr<dialogue::CancellationToken> cancel;
    dialogue::TurnId turn;
    std::string lang;
};

struct LlmCallbacks {
    std::function<void(std::string token)> on_token;
    std::function<void(LlmFinish)> on_finished;
};

class LlmClient {
public:
    LlmClient(const Config& cfg, event::EventBus& bus);
    void submit(LlmRequest, LlmCallbacks);
    bool probe();             // GET /health
    void keep_alive();        // 1-token completion, used by M2
};
```

SSE parser handles `data: {...}\n\n` chunks, the `[DONE]` sentinel, and OpenAI-compatible `choices[0].delta.content`. Cancellation aborts via `CURLOPT_WRITEFUNCTION` returning 0.

Tests: cpp-httplib in-process fake server emitting known SSE byte sequences; cancellation mid-stream finishes within 100 ms.

### C.1.3 Dialogue Manager (the active glue)

`src/dialogue/manager.{hpp,cpp}`.

The FSM stays a pure state machine. The Manager subscribes to `FinalTranscript` and runs:

1. `PromptBuilder::build()` (sync; reads memory).
2. `LlmClient::submit()` with the active turn's `CancellationToken`.
3. `on_token` → `SentenceSplitter::push` → publish `LlmSentence` events for each complete sentence.
4. `on_finished` → publish `LlmFinished`. If cancelled, the FSM's existing handler does the right thing.

### C.1.4 Turn writer

`src/dialogue/turn_writer.{hpp,cpp}`. Listens to FSM outcomes (mirrors the metrics observer) and posts writes to the MemoryThread:

- `committed`: write assistant turn with concatenated sentence text.
- `interrupted`: write only the played-out portion (sentences whose `PlaybackFinished` was observed).
- `discarded`: do not persist.

User turns get written when `FinalTranscript` arrives.

## C.2 Slice 3 — Async summarization + JSON logs

### C.2.1 Summarizer stub

`src/memory/summarizer.{hpp,cpp}`. Triggered by `cfg.memory.summary.trigger`:

- Runs on the memory thread.
- Calls `LlmClient::submit` with a compact prompt.
- Writes `summaries` row with the result and `source_hash`.

Prompt iteration is M8 work; M1 lands the machinery only.

### C.2.2 JSON structured logging

Replace M0's structured-text output with a custom spdlog sink emitting per-line JSON:

```json
{"ts":"2026-04-30T12:34:56.789+04:00","level":"info","component":"dialogue","event":"llm_first_token","turn_id":42,"latency_ms":381}
```

`src/log/json_sink.{hpp,cpp}` — `spdlog::sinks::base_sink<std::mutex>::sink_it_` impl. Adds a richer logging API:

```cpp
void event(std::string_view component, std::string_view event_name,
           dialogue::TurnId turn,
           std::initializer_list<std::pair<std::string_view, std::string>> kv);
```

Existing `info(component, message)` call sites stay unchanged.

---

## Acceptance for M1 (full)

1. With `docker compose up` running, `./build/dev/lclva --config config/dev.yaml` accepts text input on stdin, streams an answer logged as `LlmSentence` events, and writes both turns to SQLite with `status='committed'`.
2. Killing the orchestrator mid-turn and restarting it: recovery sweep marks the in-flight turn `interrupted`, the next session opens cleanly, prior session's `ended_at` set.
3. `tests/test_sentence_splitter.cpp` golden corpus passes. ✅ (slice 1)
4. SSE cancellation: setting the turn's cancellation token mid-stream causes the LLM client to abort and emit `LlmFinished{ cancelled=true }` within 100 ms.
5. Logs are valid JSON, one event per line.
6. `voice_llm_first_token_ms` and `voice_llm_tokens_per_sec` metrics emit non-zero values.

## Risks specific to M1

| Risk | Mitigation |
|---|---|
| GPU passthrough fragility on host upgrades | Document NVIDIA Container Toolkit version pin in compose README; smoke-test command in B.7 |
| Compose-image build slow on CI/contributors | Pin upstream commits; pre-built images in a personal registry are an option once we have one |
| SSE parser misses or duplicates events | Golden test fixture with captured llama.cpp SSE bytes |
| Prompt assembly slow at deep context | Token-estimate cap; trigger summarization on overflow |
| Memory thread saturates on bursty writes | Bounded write queue (default 256); DropNewest + metric on overflow |
| JSON logger performance | spdlog async logger; reusable buffer in the sink |
| Container-host time skew | Negligible; HTTP timestamps are server-authoritative |

## Time breakdown

| Part / step | Estimate | Status |
|---|---|---|
| **A** Slice 1 (memory, splitter, config) | 5 days | ✅ landed |
| **B** Compose stack (upstream images, no Dockerfiles) | ~1 day | ✅ landed |
| **C.1.1** Prompt builder | 1 day | ✅ landed |
| **C.1.2** LLM client | 2 days | ✅ landed |
| **C.1.3** Dialogue Manager | 1 day | ✅ landed |
| **C.1.4** Turn writer | 0.5 day | ✅ landed |
| **C.2.1** Summarizer stub | 0.5 day | ✅ landed |
| **C.2.2** JSON logging | 1 day | ✅ landed |
| **Total** | **~13 days = ~2.5 weeks** | **M1 complete** |
