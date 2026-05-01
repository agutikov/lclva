# Built-in Web UI Architecture

## 1. Goal

Add a first-party Web UI served by the `acva` control plane as part of the same local process and, in production, the same binary.

The UI should support:

- chat and live dialogue inspection
- configuration editing and validation
- infrastructure monitoring and management
- model, voice, disk, GPU, and runtime resource management
- memory, session, privacy, and data lifecycle operations
- audio, VAD, AEC, and playback diagnostics
- tools and access management when those backend capabilities exist
- developer diagnostics for logs, metrics, events, traces, and debug bundles

The UI must preserve the core project constraints from `project_design.md`: local-first operation, explicit backpressure, observable control-plane behavior, structural cancellation, and no direct frontend dependency on internal C++ domain objects.

## 2. Design Principles

1. **Local-first by default.** The default bind remains `127.0.0.1`; remote access requires explicit configuration and access controls.
2. **Versioned API boundary.** UI APIs live under `/api/v1`; existing script-friendly routes such as `/status`, `/metrics`, and `/health` remain stable.
3. **DTO facade, not object leakage.** HTTP handlers expose stable data-transfer objects, not internal classes or raw database rows.
4. **Realtime is event-driven.** Use Server-Sent Events first for browser updates; add WebSocket only for bidirectional realtime features that require it.
5. **Long work is asynchronous.** Downloads, imports, service restarts, audio tests, debug bundles, and benchmarks return operation IDs.
6. **Safe configuration editing.** The UI distinguishes hot-reloadable fields from restart-required fields and always validates before applying changes.
7. **Destructive actions are guarded.** Wipe, delete, restart, stop, token creation, and remote binding changes require explicit confirmation and access checks.
8. **Production can be self-contained.** Static UI assets can be embedded into the binary while development can use Vite/disk serving.

## 3. High-Level Components

```text
Browser SPA
├── Static asset requests
├── JSON API requests
└── Realtime event stream

ControlServer
├── StaticAssetServer
│   ├── EmbeddedAssetStore
│   └── DiskAssetStore (dev)
├── WebApi
│   ├── ApiResponse / ApiError helpers
│   ├── DashboardApi
│   ├── ConfigApi
│   ├── ServicesApi
│   ├── ResourcesApi
│   ├── DialogueApi
│   ├── SessionsApi
│   ├── MemoryApi
│   ├── ToolsApi
│   ├── AccessApi
│   ├── AudioApi
│   ├── PrivacyApi
│   └── DiagnosticsApi
├── WebEvents
│   ├── SSE client registry
│   ├── topic filtering
│   └── heartbeat / replay cursor
└── WebFacade
    ├── Dialogue facade
    ├── Config facade
    ├── Supervisor facade
    ├── Memory facade
    ├── Metrics facade
    ├── Resource facade
    ├── Access facade
    └── Diagnostics facade
```

The `WebFacade` layer is the compatibility boundary between the browser API and project internals. It should convert domain-specific snapshots into browser-safe DTOs, redact sensitive fields, and reject actions that are unavailable in the current milestone or deployment mode.

## 4. Static Asset Serving

### 4.1 Production Mode

Frontend build output:

```text
web/dist/
├── index.html
├── favicon.ico
├── manifest.webmanifest
└── assets/
    ├── app.[hash].js
    ├── vendor.[hash].js
    └── styles.[hash].css
```

Build pipeline:

```text
web source → frontend build → asset manifest → embedded object/static array → acva binary
```

Recommended first implementation:

- generate a C++ source file containing compressed or uncompressed asset byte arrays
- generate a manifest containing path, MIME type, size, hash, cache policy, and byte range
- link the generated object only when `ACVA_ENABLE_WEB_UI=ON`

Later optimization:

- package `web/dist` as a ZIP archive
- parse the central directory into an in-memory lookup index
- decompress requested files only
- optionally precompress `.js`, `.css`, and `.html` with gzip or Brotli if supported by the serving layer

### 4.2 Development Mode

Preferred development workflow:

```text
Browser → Vite dev server → proxy /api and /events to acva control plane
```

Alternative workflow:

```text
Browser → acva control plane → serve web/dist from disk
```

### 4.3 Static Routes

| Route | Behavior |
|---|---|
| `/` | Redirect or serve UI shell |
| `/ui` | Serve SPA shell |
| `/ui/*` | Serve concrete asset if found, otherwise SPA fallback |
| `/assets/*` | Serve hashed static assets with immutable cache |
| `/favicon.ico` | Serve favicon |
| `/manifest.webmanifest` | Serve web app manifest |

Static serving rules:

- Never SPA-fallback `/api/*`, `/events/*`, `/metrics`, `/status`, or `/health`.
- If an unknown path accepts HTML, serve `index.html`.
- If an unknown path does not accept HTML, return `404`.
- Hashed assets use `Cache-Control: public, max-age=31536000, immutable`.
- `index.html` uses `Cache-Control: no-cache`.
- All static responses should include `ETag`.

## 5. UI Information Architecture

### 5.1 MVP Navigation

```text
Home
Chat
Services
Configuration
Resources
Audio Lab
Memory
Privacy
Diagnostics
```

### 5.2 Full Page Tree

```text
Built-in Web UI
├── Home / Command Center
├── Chat / Dialogue
│   ├── Live chat
│   ├── Current voice turn
│   ├── Turn trace
│   └── Transcript export
├── Sessions / Memory
│   ├── Sessions
│   ├── Turns
│   ├── Summaries
│   ├── Facts
│   └── Recovery state
├── Configuration
│   ├── Form editor
│   ├── YAML editor
│   ├── Schema browser
│   ├── Validation results
│   ├── Snapshots
│   └── Rollback
├── Services
│   ├── Orchestrator
│   ├── LLM
│   ├── STT
│   ├── TTS
│   ├── Probe history
│   ├── Logs
│   └── Management actions
├── Resources
│   ├── LLM models
│   ├── Whisper models
│   ├── VAD models
│   ├── Piper voices
│   ├── Downloads/imports
│   ├── Disk usage
│   └── GPU fit checks
├── Audio Lab
│   ├── Devices
│   ├── Levels
│   ├── VAD live graph
│   ├── AEC diagnostics
│   ├── Playback test
│   └── Fixture recorder
├── Prompt Studio
│   ├── System prompts
│   ├── Style presets
│   ├── Prompt preview
│   └── Regression examples
├── Tools
│   ├── Registry
│   ├── Permissions
│   ├── Test console
│   └── Execution history
├── Access
│   ├── Policy
│   ├── Users
│   ├── Roles
│   ├── API tokens
│   └── Audit log
├── Evaluation
│   ├── Smoke tests
│   ├── Benchmarks
│   ├── Audio fixture tests
│   ├── Conversation replay
│   └── Soak tests
├── Privacy
│   ├── Data inventory
│   ├── Export
│   ├── Retention
│   ├── Delete session
│   └── Wipe all
├── Alerts
│   ├── Rules
│   ├── Channels
│   └── History
├── Diagnostics
│   ├── Logs
│   ├── Metrics explorer
│   ├── Raw event stream
│   ├── API explorer
│   ├── Build info
│   ├── Feature flags
│   └── Debug bundle
└── Help
    ├── Setup checklist
    ├── Model download guide
    ├── Audio setup guide
    ├── Docker Compose guide
    ├── systemd guide
    └── Troubleshooting
```

## 6. Browser Routes

| UI Route | Purpose | Primary APIs |
|---|---|---|
| `/ui` | Command center | `/api/v1/dashboard`, `/events/v1` |
| `/ui/chat` | Live chat and dialogue state | `/api/v1/dialogue/*`, `/events/v1` |
| `/ui/sessions` | Session and transcript browser | `/api/v1/sessions/*` |
| `/ui/memory` | Facts, summaries, preferences | `/api/v1/memory/*` |
| `/ui/config` | Configuration editor | `/api/v1/config/*` |
| `/ui/services` | Service monitoring and management | `/api/v1/services/*` |
| `/ui/resources` | Models, voices, disk, GPU | `/api/v1/resources/*` |
| `/ui/audio` | Audio diagnostics | `/api/v1/audio/*` |
| `/ui/prompts` | Prompt and personality studio | `/api/v1/prompts/*` |
| `/ui/tools` | Tool registry and permissions | `/api/v1/tools/*` |
| `/ui/access` | Access management | `/api/v1/access/*` |
| `/ui/evaluation` | Tests and benchmarks | `/api/v1/evaluation/*` |
| `/ui/privacy` | Data lifecycle and privacy | `/api/v1/privacy/*` |
| `/ui/alerts` | Alert configuration and history | `/api/v1/alerts/*` |
| `/ui/diagnostics` | Logs, metrics, events, debug bundles | `/api/v1/diagnostics/*` |
| `/ui/help` | Setup and troubleshooting | static docs, `/api/v1/diagnostics/build` |

## 7. API Conventions

### 7.1 Versioning

All Web UI APIs use `/api/v1`. Future incompatible changes move to `/api/v2`. The UI can query `/api/v1/version` to verify compatibility.

### 7.2 Response Envelope

```text
ApiResponse<T>
├── ok: boolean
├── data: T or null
├── error: ApiError or null
└── request_id: string
```

### 7.3 Error Model

```text
ApiError
├── code: string
├── message: string
├── details: object
└── field_errors: FieldError[]

FieldError
├── path: string
├── code: string
└── message: string
```

Common error codes:

- `validation_failed`
- `not_found`
- `conflict`
- `stale_revision`
- `restart_required`
- `service_unhealthy`
- `operation_denied`
- `operation_timeout`
- `feature_not_available`
- `internal_error`

### 7.4 HTTP Status Mapping

| Case | Status |
|---|---:|
| Success | 200 |
| Created resource or operation | 201 |
| Accepted async operation | 202 |
| Validation error | 400 |
| Unauthorized | 401 |
| Forbidden | 403 |
| Missing resource | 404 |
| Conflict or stale revision | 409 |
| Unsupported feature | 501 |
| Backend unavailable | 503 |

### 7.5 Revisioned Resources

Mutable resources use optimistic concurrency.

```text
RevisionedResource<T>
├── revision: string
├── updated_at: string
└── value: T
```

Mutation requests include either a revision field or an `If-Match` header.

## 8. Realtime API

### 8.1 Transport

MVP uses Server-Sent Events:

```text
GET /events/v1?topics=status,services,dialogue,metrics,logs
```

Add WebSocket later if the UI needs bidirectional realtime control beyond normal HTTP commands.

### 8.2 Event Envelope

```text
UiEvent
├── id: string
├── ts: string
├── topic: string
├── type: string
├── severity: trace | debug | info | warn | error
├── turn_id: integer or null
├── service: string or null
└── payload: object
```

### 8.3 Event Topics

| Topic | Example Event Types |
|---|---|
| `status` | `fsm_state_changed`, `pipeline_state_changed` |
| `services` | `service_health_changed`, `probe_failed`, `restart_observed` |
| `dialogue` | `turn_started`, `assistant_token`, `sentence_queued`, `playback_started`, `turn_committed` |
| `metrics` | `queue_depth_changed`, `latency_sample`, `resource_pressure_changed` |
| `logs` | `log_line` |
| `config` | `config_reloaded`, `config_validation_failed` |
| `resources` | `download_progress`, `asset_validated` |
| `audio` | `vad_probability`, `aec_delay_estimate`, `playback_underrun` |

### 8.4 SSE Operational Rules

- Send heartbeat comments every 15 seconds.
- Support topic filtering to avoid flooding the UI.
- Apply per-client bounded queues.
- Drop oldest non-critical events when a client is slow.
- Always keep status/service/config events; logs and high-rate metrics can be lossy.

## 9. API Route Catalog and Data Models

### 9.1 Health, Version, Status, Dashboard

Routes:

```text
GET /api/v1/health
GET /api/v1/version
GET /api/v1/status
GET /api/v1/dashboard
```

`VersionInfo`:

```text
VersionInfo
├── api_version: string
├── app_version: string
├── git_commit: string
├── build_type: string
├── schema_version: string
├── frontend_version: string
└── enabled_features: string[]
```

`StatusSnapshot`:

```text
StatusSnapshot
├── state: string
├── active_turn: integer
├── outcome: string
├── sentences_played: integer
├── turns_completed: integer
├── turns_interrupted: integer
├── turns_discarded: integer
├── pipeline_state: ok | degraded | failed | unconfigured
├── services: ServiceSummary[]
├── queues: QueueSummary[]
├── resources: ResourcePressure
└── updated_at: string
```

`DashboardSnapshot`:

```text
DashboardSnapshot
├── status: StatusSnapshot
├── alerts: AlertSummary[]
├── recent_sessions: SessionSummary[]
├── metrics: MetricSummary
├── config: ConfigSummary
└── quick_actions: QuickAction[]
```

`QuickAction`:

```text
QuickAction
├── id: string
├── label: string
├── method: GET | POST | PATCH | DELETE
├── endpoint: string
├── enabled: boolean
├── destructive: boolean
└── disabled_reason: string or null
```

### 9.2 Configuration API

Routes:

```text
GET /api/v1/config
GET /api/v1/config/schema
POST /api/v1/config/validate
PATCH /api/v1/config
POST /api/v1/config/reload
GET /api/v1/config/snapshots
POST /api/v1/config/snapshots
POST /api/v1/config/rollback
```

`ConfigDocument`:

```text
ConfigDocument
├── revision: string
├── path: string
├── effective: object
├── source_yaml: string
├── schema_version: string
├── fields: ConfigFieldMeta[]
├── validation: ConfigValidationResult
└── restart_required: boolean
```

`ConfigFieldMeta`:

```text
ConfigFieldMeta
├── path: string
├── type: string
├── description: string
├── hot_reloadable: boolean
├── restart_required: boolean
├── secret: boolean
├── default_value: any
├── current_value: any
└── allowed_values: any[]
```

`ConfigValidationResult`:

```text
ConfigValidationResult
├── valid: boolean
├── errors: FieldError[]
├── warnings: FieldError[]
├── restart_required_paths: string[]
└── hot_reloadable_paths: string[]
```

`ConfigPatchRequest`:

```text
ConfigPatchRequest
├── revision: string
├── patch: object
├── validate_only: boolean
└── apply_reload: boolean
```

UI requirements:

- show both form and raw YAML editor
- validate before save
- show restart-required warnings inline
- redact secret fields
- snapshot before every successful write
- support rollback from snapshot

### 9.3 Services API

Routes:

```text
GET /api/v1/services
GET /api/v1/services/{service_id}
POST /api/v1/services/{service_id}/probe
POST /api/v1/services/{service_id}/start
POST /api/v1/services/{service_id}/stop
POST /api/v1/services/{service_id}/restart
GET /api/v1/services/{service_id}/logs
GET /api/v1/services/{service_id}/metrics
POST /api/v1/services/{service_id}/warmup
```

`ServiceSummary`:

```text
ServiceSummary
├── id: llama | whisper | piper | orchestrator
├── display_name: string
├── kind: llm | stt | tts | orchestrator
├── state: not_configured | probing | healthy | degraded | unhealthy | restarting | disabled
├── health_url: string
├── base_url: string
├── unit: string or null
├── deployment: compose | systemd | external | none
├── last_probe_at: string or null
├── last_probe_latency_ms: number or null
├── consecutive_failures: integer
├── fail_pipeline_if_down: boolean
├── restart_count: integer
├── capabilities: ServiceCapability
└── message: string
```

`ServiceCapability`:

```text
ServiceCapability
├── can_probe: boolean
├── can_start: boolean
├── can_stop: boolean
├── can_restart: boolean
├── can_tail_logs: boolean
├── can_warmup: boolean
└── reason_if_disabled: string or null
```

Service management policy:

- Compose/dev mode starts with probe, metrics, warmup, and logs where available.
- systemd mode can add start, stop, restart through the optional bus integration.
- external mode exposes status/probe only.

### 9.4 Resources API

Routes:

```text
GET /api/v1/resources/overview
GET /api/v1/resources/models
POST /api/v1/resources/models/import
POST /api/v1/resources/models/download
DELETE /api/v1/resources/models/{model_id}
POST /api/v1/resources/models/{model_id}/validate
GET /api/v1/resources/voices
POST /api/v1/resources/voices/import
POST /api/v1/resources/voices/download
DELETE /api/v1/resources/voices/{voice_id}
POST /api/v1/resources/voices/{voice_id}/preview
GET /api/v1/resources/disk
GET /api/v1/resources/gpu
```

`ModelAsset`:

```text
ModelAsset
├── id: string
├── kind: llm | stt | vad
├── name: string
├── path: string
├── size_bytes: integer
├── format: gguf | bin | onnx | unknown
├── quantization: string or null
├── context_tokens: integer or null
├── language: string or null
├── checksum: string or null
├── installed: boolean
├── configured: boolean
├── compatible: boolean
└── issues: string[]
```

`VoiceAsset`:

```text
VoiceAsset
├── id: string
├── language: string
├── name: string
├── path: string
├── config_path: string
├── size_bytes: integer
├── configured_url: string or null
├── is_fallback: boolean
├── preview_available: boolean
└── issues: string[]
```

`ResourcePressure`:

```text
ResourcePressure
├── cpu_percent: number or null
├── memory_used_bytes: integer or null
├── memory_total_bytes: integer or null
├── gpu: GpuSummary[]
├── disk: DiskUsageSummary[]
└── warnings: string[]
```

`GpuSummary`:

```text
GpuSummary
├── id: string
├── name: string
├── memory_used_bytes: integer
├── memory_total_bytes: integer
├── utilization_percent: number or null
├── temperature_c: number or null
└── warnings: string[]
```

`DiskUsageSummary`:

```text
DiskUsageSummary
├── path: string
├── label: models | voices | db | logs | recordings | config | other
├── used_bytes: integer
├── available_bytes: integer or null
└── warnings: string[]
```

### 9.5 Dialogue API

Routes:

```text
GET /api/v1/dialogue/state
POST /api/v1/dialogue/message
POST /api/v1/dialogue/interrupt
POST /api/v1/dialogue/mute
POST /api/v1/dialogue/unmute
POST /api/v1/dialogue/new-session
GET /api/v1/dialogue/current-turn
GET /api/v1/dialogue/trace/{turn_id}
```

`ChatMessageRequest`:

```text
ChatMessageRequest
├── text: string
├── lang: string or null
├── session_id: integer or null
├── mode: text | voice_simulated
└── options: object
```

`DialogueState`:

```text
DialogueState
├── fsm_state: string
├── active_turn: integer
├── muted: boolean
├── current_session_id: integer or null
├── current_user_text: string
├── current_assistant_text: string
├── outcome: string
└── updated_at: string
```

`TurnTrace`:

```text
TurnTrace
├── turn_id: integer
├── session_id: integer
├── state: string
├── user_text: string
├── assistant_text: string
├── lang: string
├── speculative: boolean
├── timings: TurnTiming
├── events: UiEvent[]
└── persistence_status: committed | interrupted | discarded | in_progress
```

`TurnTiming`:

```text
TurnTiming
├── speech_started_at: string or null
├── speech_ended_at: string or null
├── stt_final_at: string or null
├── llm_started_at: string or null
├── first_token_at: string or null
├── first_sentence_at: string or null
├── tts_started_at: string or null
├── first_audio_at: string or null
├── playback_started_at: string or null
└── playback_ended_at: string or null
```

### 9.6 Sessions and Memory API

Routes:

```text
GET /api/v1/sessions
POST /api/v1/sessions
GET /api/v1/sessions/{session_id}
GET /api/v1/sessions/{session_id}/turns
GET /api/v1/sessions/{session_id}/export
DELETE /api/v1/sessions/{session_id}
GET /api/v1/memory/summaries
GET /api/v1/memory/facts
POST /api/v1/memory/facts
PATCH /api/v1/memory/facts/{fact_id}
DELETE /api/v1/memory/facts/{fact_id}
GET /api/v1/memory/search
POST /api/v1/memory/rebuild-summary
```

`SessionSummary`:

```text
SessionSummary
├── id: integer
├── title: string
├── started_at: string
├── ended_at: string or null
├── turn_count: integer
├── dominant_lang: string
├── status: active | ended | recovered
└── last_turn_preview: string
```

`TurnRecord`:

```text
TurnRecord
├── id: integer
├── session_id: integer
├── role: user | assistant
├── text: string
├── lang: string
├── started_at: string
├── ended_at: string or null
├── status: in_progress | committed | interrupted | discarded
├── interrupted_at_sentence: integer or null
└── audio_path: string or null
```

`FactRecord`:

```text
FactRecord
├── id: integer
├── key: string
├── value: string
├── lang: string or null
├── source_turn_id: integer or null
├── confidence: number
└── updated_at: string
```

`SummaryRecord`:

```text
SummaryRecord
├── id: integer
├── session_id: integer
├── range_start_turn: integer
├── range_end_turn: integer
├── summary: string
├── lang: string
├── source_hash: string
└── created_at: string
```

### 9.7 Tools API

Routes:

```text
GET /api/v1/tools
GET /api/v1/tools/{tool_id}
PATCH /api/v1/tools/{tool_id}
POST /api/v1/tools/{tool_id}/test
GET /api/v1/tools/executions
GET /api/v1/tools/permissions
PATCH /api/v1/tools/permissions
```

`ToolDefinition`:

```text
ToolDefinition
├── id: string
├── display_name: string
├── description: string
├── enabled: boolean
├── source: builtin | mcp | local_script | http
├── schema: object
├── permissions: string[]
├── risk_level: low | medium | high
└── last_used_at: string or null
```

`ToolExecution`:

```text
ToolExecution
├── id: string
├── tool_id: string
├── started_at: string
├── finished_at: string or null
├── status: running | succeeded | failed | cancelled
├── input_preview: object
├── output_preview: object or null
└── error: ApiError or null
```

Tools are post-MVP unless tool calling is added. The current project design explicitly keeps tool calling out of MVP scope.

### 9.8 Access API

Routes:

```text
GET /api/v1/access/policy
PATCH /api/v1/access/policy
GET /api/v1/access/users
POST /api/v1/access/users
PATCH /api/v1/access/users/{user_id}
DELETE /api/v1/access/users/{user_id}
GET /api/v1/access/tokens
POST /api/v1/access/tokens
DELETE /api/v1/access/tokens/{token_id}
GET /api/v1/access/audit
```

`AccessPolicy`:

```text
AccessPolicy
├── bind_mode: localhost_only | lan | public
├── auth_required: boolean
├── allowed_origins: string[]
├── allow_destructive_actions: boolean
├── require_admin_for_wipe: boolean
├── session_timeout_minutes: integer
└── setup_completed: boolean
```

`UserRecord`:

```text
UserRecord
├── id: string
├── display_name: string
├── role: viewer | operator | admin
├── enabled: boolean
├── created_at: string
└── last_seen_at: string or null
```

`ApiTokenRecord`:

```text
ApiTokenRecord
├── id: string
├── name: string
├── role: viewer | operator | admin
├── created_at: string
├── expires_at: string or null
└── last_used_at: string or null
```

`AuditEvent`:

```text
AuditEvent
├── id: string
├── ts: string
├── actor: string
├── action: string
├── target: string
├── outcome: allowed | denied | failed
└── details: object
```

### 9.9 Audio Lab API

Routes:

```text
GET /api/v1/audio/devices
POST /api/v1/audio/test-output
POST /api/v1/audio/test-input
GET /api/v1/audio/vad/state
POST /api/v1/audio/vad/calibrate
GET /api/v1/audio/aec/state
POST /api/v1/audio/aec/test
GET /api/v1/audio/levels
POST /api/v1/audio/record-fixture
```

`AudioDevice`:

```text
AudioDevice
├── id: string
├── name: string
├── kind: input | output | duplex
├── default: boolean
├── sample_rates: integer[]
├── channels: integer
└── selected: boolean
```

`AudioLevels`:

```text
AudioLevels
├── input_rms_db: number or null
├── input_peak_db: number or null
├── output_rms_db: number or null
├── output_peak_db: number or null
├── clipping: boolean
└── updated_at: string
```

`VadState`:

```text
VadState
├── probability: number
├── threshold_onset: number
├── threshold_offset: number
├── state: silence | maybe_speech | speech | hangover
├── false_starts_total: integer
└── last_speech_at: string or null
```

`AecState`:

```text
AecState
├── enabled: boolean
├── reference_present: boolean
├── delay_estimate_ms: number
├── suppression_db: number or null
├── alignment_status: ok | drifting | missing_reference | unknown
└── issues: string[]
```

### 9.10 Prompt Studio API

Routes:

```text
GET /api/v1/prompts
PATCH /api/v1/prompts/{lang}
POST /api/v1/prompts/preview
GET /api/v1/prompts/snapshots
POST /api/v1/prompts/snapshots
POST /api/v1/prompts/rollback
```

`PromptTemplate`:

```text
PromptTemplate
├── lang: string
├── system_prompt: string
├── style_preset: string or null
├── updated_at: string
└── revision: string
```

`PromptPreview`:

```text
PromptPreview
├── lang: string
├── assembled_prompt: string
├── token_estimate: integer
├── included_facts: FactRecord[]
├── included_summary: SummaryRecord or null
└── included_turns: TurnRecord[]
```

### 9.11 Evaluation API

Routes:

```text
GET /api/v1/evaluation/smoke-tests
POST /api/v1/evaluation/smoke-tests/run
GET /api/v1/evaluation/benchmarks
POST /api/v1/evaluation/benchmarks/run
GET /api/v1/evaluation/soak-tests
POST /api/v1/evaluation/soak-tests/run
GET /api/v1/evaluation/audio-fixtures
POST /api/v1/evaluation/audio-fixtures/run
```

`EvaluationRun`:

```text
EvaluationRun
├── id: string
├── kind: smoke | benchmark | soak | audio_fixture | replay
├── status: queued | running | succeeded | failed | cancelled
├── started_at: string
├── finished_at: string or null
├── summary: string
├── metrics: object
└── artifacts: ArtifactRef[]
```

### 9.12 Privacy API

Routes:

```text
GET /api/v1/privacy/inventory
GET /api/v1/privacy/export
POST /api/v1/privacy/delete-session
POST /api/v1/privacy/wipe-all
PATCH /api/v1/privacy/retention
GET /api/v1/privacy/network-audit
```

`DataInventory`:

```text
DataInventory
├── sessions_count: integer
├── turns_count: integer
├── summaries_count: integer
├── facts_count: integer
├── db_size_bytes: integer
├── audio_recordings_size_bytes: integer
├── logs_size_bytes: integer
├── models_size_bytes: integer
└── voices_size_bytes: integer
```

`RetentionPolicy`:

```text
RetentionPolicy
├── keep_transcripts_days: integer or null
├── keep_audio_days: integer or null
├── keep_logs_days: integer or null
├── redact_pii_in_logs: boolean
└── audio_recording_enabled: boolean
```

### 9.13 Alerts API

Routes:

```text
GET /api/v1/alerts
GET /api/v1/alerts/rules
POST /api/v1/alerts/rules
PATCH /api/v1/alerts/rules/{rule_id}
DELETE /api/v1/alerts/rules/{rule_id}
GET /api/v1/alerts/history
```

`AlertRule`:

```text
AlertRule
├── id: string
├── name: string
├── enabled: boolean
├── severity: info | warn | error
├── condition: string
├── cooldown_seconds: integer
└── channels: string[]
```

`AlertSummary`:

```text
AlertSummary
├── id: string
├── ts: string
├── severity: info | warn | error
├── title: string
├── message: string
├── source: string
└── acknowledged: boolean
```

### 9.14 Diagnostics API

Routes:

```text
GET /api/v1/diagnostics/logs
GET /api/v1/diagnostics/events
GET /api/v1/diagnostics/metrics
GET /api/v1/diagnostics/build
POST /api/v1/diagnostics/debug-bundle
GET /api/v1/diagnostics/config-schema
GET /api/v1/diagnostics/recovery-state
```

`BuildInfo`:

```text
BuildInfo
├── version: string
├── git_commit: string
├── build_type: string
├── compiler: string
├── enabled_features: string[]
├── frontend_version: string
└── schema_version: string
```

`LogEntry`:

```text
LogEntry
├── ts: string
├── level: trace | debug | info | warn | error | critical
├── component: string
├── event: string
├── turn_id: integer or null
├── message: string
└── fields: object
```

## 10. Operations API

Routes:

```text
GET /api/v1/operations
GET /api/v1/operations/{operation_id}
POST /api/v1/operations/{operation_id}/cancel
```

`Operation`:

```text
Operation
├── id: string
├── kind: string
├── status: queued | running | succeeded | failed | cancelled
├── progress: number
├── message: string
├── started_at: string
├── finished_at: string or null
├── result: object or null
└── error: ApiError or null
```

Operations are used for:

- model downloads
- voice downloads
- model and voice imports
- checksum validation
- service restart/start/stop when available
- warmup probes
- audio calibration/tests
- debug bundle creation
- benchmarks and soak tests
- privacy export and wipe actions

## 11. Data Ownership and Threading Boundaries

| Domain | Owner | UI Access Pattern |
|---|---|---|
| FSM/dialogue | Dialogue Manager | query snapshot, send commands, subscribe events |
| Services | Supervisor | query state, probe, capability-gated management |
| Config | Config subsystem | read effective config, validate, patch, reload |
| Memory | Repository / memory thread | async-safe query commands; HTTP never directly mutates SQLite |
| Metrics | Metrics registry | scrape `/metrics`; expose JSON summaries for UI |
| Logs | Logging subsystem | bounded ring-buffer tail and event stream |
| Resources | Asset service | scan configured directories, validate checksums, start operations |
| Access | Access policy service | gate destructive and remote-capable APIs |
| Audio | Audio pipeline | snapshots and diagnostic streams only; no blocking from HTTP thread |

Rules:

- HTTP handlers must be short and non-blocking.
- Long operations must run off the HTTP server thread and report through `Operation` plus events.
- Memory reads and writes go through the repository/memory thread boundary.
- High-rate audio metrics are sampled or downsampled before reaching the browser.
- Event streams use bounded queues per client.

## 12. Security and Access Model

### 12.1 Phase 1: Local-Only

- Bind to `127.0.0.1` by default.
- No authentication required for read-only local APIs.
- Destructive local APIs require explicit confirmation payloads.
- CORS disabled by default.
- No cookies required for localhost mode.

### 12.2 Phase 2: Remote-Capable

- Binding to LAN or public addresses requires setup completion.
- Authentication is mandatory outside localhost.
- Add role-based permissions: `viewer`, `operator`, `admin`.
- Add CSRF protection for browser sessions.
- Add API tokens for automation.
- Add audit events for management and destructive actions.
- Require admin role for wipe, token management, access policy changes, and service stop/restart.

### 12.3 Permission Matrix

| Capability | Viewer | Operator | Admin |
|---|---:|---:|---:|
| View dashboard | yes | yes | yes |
| View logs/metrics | yes | yes | yes |
| Chat | yes | yes | yes |
| Mute/unmute | no | yes | yes |
| Reload config | no | yes | yes |
| Edit config | no | no | yes |
| Probe services | yes | yes | yes |
| Restart services | no | yes | yes |
| Manage resources | no | yes | yes |
| Delete sessions | no | no | yes |
| Wipe all data | no | no | yes |
| Manage access | no | no | yes |

## 13. Configuration Additions

Proposed config keys:

```yaml
control:
  bind: "127.0.0.1"
  port: 9876
  web:
    enabled: true
    mode: embedded        # embedded | disk | disabled
    dev_static_dir: ""    # used when mode=disk
    spa_fallback: true
    api_prefix: "/api/v1"
    events_path: "/events/v1"
    require_auth: false
    allow_remote_setup: false
    cors:
      enabled: false
      allowed_origins: []
```

Hot-reloadable:

- `control.web.enabled` when disabling UI routes only, not the server listener
- `control.web.require_auth`
- `control.web.cors.enabled`
- `control.web.cors.allowed_origins`

Restart-required:

- `control.bind`
- `control.port`
- `control.web.mode`
- `control.web.dev_static_dir`

## 14. Backend Module Layout

Proposed C++ files:

```text
src/http/server.hpp
src/http/server.cpp
src/http/web_assets.hpp
src/http/web_assets.cpp
src/http/web_api.hpp
src/http/web_api.cpp
src/http/web_events.hpp
src/http/web_events.cpp
src/http/api_response.hpp
src/http/api_errors.hpp
src/http/api_types.hpp
src/http/mime.hpp
src/http/static_manifest.hpp
```

Proposed frontend files:

```text
web/package.json
web/vite.config.ts
web/src/main.ts
web/src/routes/
web/src/lib/api/
web/src/lib/events/
web/src/lib/stores/
web/src/lib/components/
web/src/lib/types/
```

Feature flag:

```text
ACVA_ENABLE_WEB_UI
```

When disabled:

- no frontend build required
- no embedded assets linked
- `/api/v1` can still exist for automation, but `/ui` returns `404` or `501`

## 15. Frontend Architecture

Recommended stack:

- Vite
- SvelteKit static adapter or React + Vite
- TypeScript
- generated or hand-written API client
- SSE event store
- component library kept lightweight

Frontend modules:

```text
apiClient
├── request envelope handling
├── error normalization
├── revision headers
└── typed endpoint functions

eventClient
├── EventSource connection
├── reconnect/backoff
├── topic subscriptions
└── browser stores

stores
├── status
├── services
├── config
├── dialogue
├── sessions
├── resources
├── audio
└── auth/access
```

Frontend behavior:

- Use polling fallback when SSE is unavailable.
- Show stale-data indicators when disconnected.
- Avoid rendering raw untrusted HTML from transcripts or logs.
- Require typed confirmation for destructive operations like wipe-all.

## 16. Testing Strategy

Backend tests:

- API envelope serialization
- error mapping
- `/api/v1/status` compatibility with `/status`
- static asset serving
- SPA fallback behavior
- MIME type lookup
- ETag handling
- SSE heartbeat and topic filtering
- capability gating for service actions
- config validation response shape

Frontend tests:

- route rendering smoke tests
- API client error handling
- SSE reconnect behavior
- config editor restart-required warnings
- destructive action confirmation behavior

Integration tests:

- launch `acva` with web UI enabled
- fetch `/ui`
- fetch `/api/v1/status`
- subscribe to `/events/v1`
- verify `/metrics`, `/status`, and `/health` still work

## 17. Milestone Plan

```text
W0 Design
├── finalize route tree and DTOs
├── add config keys for web UI enablement
└── choose frontend stack

W1 Static UI Shell
├── add disk asset serving
├── add embedded asset abstraction
├── add SPA fallback
├── add build feature flag
└── add Home page with status fetch

W2 API Foundation
├── add /api/v1/health
├── add /api/v1/version
├── add /api/v1/status
├── add /api/v1/dashboard
├── add response envelope and error format
└── add frontend API client

W3 Realtime Foundation
├── add /events/v1 SSE
├── stream heartbeat/status/service events
├── add frontend event store
└── add reconnect/stale UI handling

W4 Configuration and Services
├── add config schema/read/validate/reload UI
├── add services health/probe UI
├── add capability-gated service actions
└── add log snippets for services

W5 Chat, Sessions, Memory
├── add text dialogue UI
├── add sessions browser
├── add turn traces
├── add facts editor
└── add summary browser

W6 Resources and Audio Lab
├── add model and voice inventory
├── add disk/resource pressure UI
├── add audio devices and VAD diagnostics
├── add AEC diagnostics
└── add voice preview

W7 Privacy, Access, Diagnostics
├── add data inventory and wipe flows
├── add local/remote access policy
├── add debug bundle generation
├── add log/event explorer
└── add API token support if remote mode is enabled
```

## 18. MVP Cut

The smallest useful built-in UI should include:

1. Static SPA shell served from `/ui`.
2. `/api/v1/health`, `/api/v1/version`, `/api/v1/status`, and `/api/v1/dashboard`.
3. `/events/v1` heartbeat plus status/service events.
4. Home page showing FSM state, service health, quick actions, and resource warnings.
5. Services page showing supervisor state and probe actions.
6. Configuration page showing current effective config, validation, and hot-reload/restart-required markers.
7. Diagnostics page showing logs, metrics links, build info, and debug bundle placeholder.

Everything else can be hidden behind feature availability flags until the corresponding backend capability lands.

## 19. Immediate Implementation Checklist

1. Add `control.web` config fields.
2. Add API response and error helpers.
3. Add `/api/v1/health`, `/api/v1/version`, and `/api/v1/status`.
4. Add `/api/v1/dashboard` using current FSM/status data plus placeholder arrays for future services/resources.
5. Add static asset abstraction with disk mode first.
6. Add embedded asset abstraction behind `ACVA_ENABLE_WEB_UI`.
7. Add `/ui` route and SPA fallback.
8. Add `/events/v1` SSE heartbeat skeleton.
9. Add tests for route behavior and JSON envelopes.
10. Add frontend shell under `web/` after the C++ foundation is stable.

## 20. Open Questions

1. Frontend stack: SvelteKit static, React + Vite, or plain Vite?
2. Should `/api/v1` exist even when `ACVA_ENABLE_WEB_UI=OFF`?
3. Should local destructive actions use confirmation payloads only, or require an admin setup even on localhost?
4. Should static assets be embedded as generated C++ arrays first or ZIP object first?
5. Should remote access be supported before M8, or explicitly deferred until production hardening?
6. Should the UI expose service stop/restart in Compose mode, or only in systemd mode?
7. Should transcript text be redacted by default in logs and diagnostics even inside the local UI?
