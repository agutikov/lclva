# M8C — Distribution & Wake-Word

**Estimate:** ~1 week.

**Depends on:** M0–M7. Sibling sub-milestones M8A (admin & state) and M8B (observability + soak); M8C touches the audio pipeline (wake-word) and the packaging surface, neither of which conflicts with the other sub-milestones.

**Blocks:** MVP release (with M8A + M8B).

## Goal

The user-facing + delivery half of M8: one new audio-pipeline feature (wake-word) and the work needed to ship the project to people who didn't write it. Four surfaces:

1. **Wake-word** — gate the VAD/STT path on a small wake-word model so the agent only acts on speech that addresses it. Default off so M5–M8B behavior is unchanged.
2. **Packaging** — Compose (dev path) and systemd (prod alternative) finalized.
3. **Documentation pass** — runbook, configuration reference, contributor architecture summary.
4. **Final sweep** — TODOs, lint, sanitizers.

The split from the original M8 is purely organizational; M8 was growing past 11 steps. This sub-milestone groups distribution + the one user-facing UX feature that fits the same release window.

## Out of scope

- Adaptive endpointer / address detection (covered by **M10 — Conversational UX**, depends on M9 partials).
- Admin / control plane (M8A).
- Soak / observability (M8B).

## Step 1 — Wake-word

A lightweight keyword spotter ahead of the VAD that lets the agent
listen passively to background speech but only feed audio into the
STT/LLM path after a configured wake phrase ("Hey acva", "OK acva",
or whatever `cfg.audio.wake_words: [...]` contains).

This complements the M5 default (always-on dialogue) for users in
shared rooms / with TVs / on conference calls — without it, every
background utterance triggers a turn.

**Default off.** When `cfg.audio.wake_word.enabled: false` (default),
the audio pipeline behaves exactly as in M5 — no extra latency, no
new dependency. Enabling it adds a ~5 MB ONNX model + ~2 ms per
32 ms VAD frame on the audio worker thread.

**Engine:** [openWakeWord](https://github.com/dscripka/openWakeWord)
ONNX models, run via the existing ONNX Runtime install (already a
dep since M4 for Silero VAD). Why this over Porcupine: Apache-2.0
license (Porcupine is paid for commercial), pre-trained models
include "hey jarvis", "alexa", "ok google" out of the box, custom
words trainable from ~50 minutes of synthesized audio.

Pipeline insertion point — `src/audio/pipeline.cpp`, after the
resampler, before the Silero VAD push:

```text
SPSC ring → resampler (48→16k) → wake-word ──────┐
                                  ↓ (gate)        │
                                Silero VAD ←──────┘
```

While the gate is closed, frames are still fed to the wake-word
model (it's always listening) but NOT to Silero/endpointer/STT —
saving STT compute + avoiding spurious turns. Gate opens on a
positive wake-word detection and stays open for
`cfg.audio.wake_word.followup_window_ms` (default 8000 ms; refreshes
on every additional VAD onset). After the window expires with no
new speech, the gate closes again.

**Files:**
- `src/audio/wake_word.hpp/cpp` — openWakeWord wrapper, parallel to
  `src/audio/vad.cpp`.
- `src/audio/pipeline.cpp` — insert the gate; thread the new
  decision into `live_sink_` and `endpointer_.on_frame` calls.
- `scripts/download-wake-word.sh` — fetch the chosen ONNX models
  (separate from `download-vad.sh` since it's an opt-in path).
- `tests/test_wake_word.cpp` — drive synthetic audio (silence,
  music, the actual wake phrase) and assert detection rates.

Config:

```yaml
audio:
  wake_word:
    enabled: false          # default
    model_paths:
      - "${HOME}/.local/share/acva/models/hey-acva.onnx"
    threshold: 0.6          # 0..1; openWakeWord's confidence
    followup_window_ms: 8000
```

**Why this lives here, not in M5:**
- M5's "always-on dialogue" is the right MVP default for a single-
  user dev workstation. Wake-word matters for multi-occupant rooms
  / production.
- Adaptive endpointer / address-detection (M10) is the more complete
  fix; wake-word is the cheap, deterministic version that ships
  alongside the MVP.

## Step 2 — Packaging

Two deployment paths ship side-by-side; both have been informally validated since M1.

### Dev path: Docker Compose (default since M1.B)

- `packaging/compose/docker-compose.yml` — already in tree from M1.B; finalized here:
  - LLM, STT (whichever M5 picked: whisper / speaches / faster-whisper), and per-language Piper services pinned to image digests (not `:latest` floating tags).
  - `.env.example` documented.
- `scripts/dev-up.sh` — `cd packaging/compose && docker compose up -d`, plus a model-availability check.
- `scripts/dev-down.sh` — symmetric.
- The orchestrator continues to run as a host CLI (`./_build/release/acva --config ...`).

### Production path: systemd units (alternative)

- `packaging/systemd/acva.service`, `acva-llama.service`, `acva-whisper.service`, `acva-piper.service`, `acva.target` — finalized; were placeholders since M2.
- `scripts/install-systemd.sh` — copies units to `~/.config/systemd/user/`, runs `systemctl --user daemon-reload`. The script defers binary install to the user (or a downstream package).
- `scripts/uninstall-systemd.sh` — symmetric.
- Switching to this path requires `cfg.supervisor.bus_kind: user` and recompilation with `-DACVA_ENABLE_SDBUS=ON` (gates the optional sd-bus client described in m2_supervision.md's "systemd alternative" section).
- `packaging/man/acva.1` — man page (terse), independent of deployment path.

### Optional (stretch)

- AUR `PKGBUILD` for Arch / Manjaro.
- `.deb` build script for Debian/Ubuntu.

## Step 3 — Documentation pass

- `README.md` — installation steps refined based on real-user experience during M0–M7. (Already partially done; needs final pass.)
- `docs/operations.md` — runbook for "the LLM is unhappy", "the mic isn't picked up", common failure modes.
- `docs/configuration.md` — full reference of every config field, with default and notes.
- `docs/architecture.md` — distilled summary for new contributors.

## Step 4 — Final sweep

- Address every TODO in the codebase (or open an issue for it).
- Run clang-tidy with the project's `.clang-tidy` config; fix or suppress.
- Make sure every public function has at least a one-line comment when the *why* is non-obvious (per CLAUDE.md guidance).
- Run the full test suite under ASan, UBSan, TSan once each.

## Acceptance

1. **Wake-word works.** With `cfg.audio.wake_word.enabled: true`, the agent silently ignores background speech (no `SpeechStarted` events on the bus). Saying the wake phrase opens the gate; the next utterance routes through the M5 STT path normally. Latency cost vs M5 default ≤ 50 ms per turn.
2. **Both deployment paths work** end-to-end on a clean Manjaro and a clean Ubuntu 24.04 VM:
   - Docker Compose: `docker compose up -d && ./scripts/dev-up.sh` brings up backends; `./_build/release/acva` connects on the host.
   - systemd: `./scripts/install-systemd.sh && systemctl --user start acva.target` brings up the full stack as units; `systemctl --user status` shows all four `active (running)`.
3. **Documentation complete.** A new contributor can read README + architecture.md and understand the system.
4. **Final sweep clean.** Zero clang-tidy errors, zero ASan/UBSan/TSan reports on the test suite.

## Risks specific to M8C

| Risk | Mitigation |
|---|---|
| Wake-word false negatives — user says wake phrase, gate doesn't open | Ship multiple model paths (custom + at least one of the openWakeWord stock words); threshold tunable in config |
| Wake-word false positives in noisy rooms | `followup_window_ms` is the bounded blast radius — at most one spurious turn per FP, not a sustained intake |
| Adding ONNX wake-word model into the audio thread breaks the realtime path | Run the wake-word inference on the audio-pipeline worker thread (the same one that already runs Silero), not on the PortAudio callback thread. Latency budget is set at <2 ms/frame with margin. |
| Packaging breaks on non-default file layouts | XDG-compliant paths; document overrides |

## Time breakdown

| Step | Estimate |
|---|---|
| 1 Wake-word | 2 days |
| 2 Packaging | 1.5 days |
| 3 Docs | 1.5 days |
| 4 Final sweep | 1 day |
| **Total** | **~6 days = ~1 week** |
