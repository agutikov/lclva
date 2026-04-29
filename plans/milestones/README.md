# Milestone plans

Detailed per-milestone plans. The high-level table lives in `../project_design.md` §17; this directory has one file per milestone with concrete steps, file lists, APIs, tests, and acceptance criteria.

| #  | File | Status      |
|----|------|-------------|
| M0 | `m0_skeleton.md` | ✅ landed   |
| M1 | `m1_llm_memory.md` | next      |
| M2 | `m2_supervision.md` | planned   |
| M3 | `m3_tts_playback.md` | planned   |
| M4 | `m4_audio_vad.md` | planned   |
| M5 | `m5_streaming_stt.md` | planned   |
| M6 | `m6_aec.md` | planned     |
| M7 | `m7_barge_in.md` | planned     |
| M8 | `m8_hardening.md` | planned    |

## Conventions used in these documents

- **Step**: an ordered chunk of work suitable for a single PR (or, in solo dev, a single coherent push).
- **Files**: paths under `src/` and `tests/` that the milestone creates or substantially modifies.
- **Acceptance**: the observable behavior that must hold before the milestone closes. Anything not listed is not in scope for this milestone.
- **Risks**: things that have a real chance of derailing the milestone, with the mitigation we've already chosen.

When a milestone surfaces a new design question, log it in `../open_questions.md` rather than mutating the milestone plan in place.
