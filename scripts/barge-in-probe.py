#!/usr/bin/env python3
"""M6B Gate 3 — barge-in audibility probe.

Watches the live `acva-*.log` file while you (the user) speak short
distinctive phrases over assistant TTS.  For each prompted attempt,
checks whether a `final_transcript` event fires within the prompt
window AND its text doesn't match the assistant's most recent
`llm_sentence` (i.e., it was you, not echo bleed-through).

This script does NOT start acva.  Workflow:
    1. start acva in another terminal (`./_build/dev/acva`)
    2. seed it with something that triggers a long TTS response
       (e.g., `acva --stdin` and type "tell me a long story")
    3. run this probe from a third terminal
    4. when prompted, speak a short distinctive phrase that is NOT
       something the assistant is saying

Reports N/M attempts at the end.  PASS criterion (M6 gate 3): 5/5.

Usage:
    python3 scripts/barge-in-probe.py                  # default: 5 attempts, 6s each
    python3 scripts/barge-in-probe.py --attempts 3
    python3 scripts/barge-in-probe.py --log-dir /tmp/acva
"""

import argparse
import glob
import json
import os
import sys
import time


def latest_log(log_dir: str) -> str | None:
    files = sorted(glob.glob(os.path.join(log_dir, "acva-*.log")))
    return files[-1] if files else None


def tail_since(path: str, offset: int) -> tuple[list[dict], int]:
    """Read everything in `path` from `offset` onwards.  Return
    (parsed JSON entries, new offset).  Lines that fail JSON parse
    are silently skipped — the log mixes JSON events with the
    occasional ALSA stderr line, and we don't want to crash on
    those."""
    out: list[dict] = []
    try:
        with open(path, "rb") as f:
            f.seek(offset)
            data = f.read()
            new_offset = f.tell()
    except OSError as e:
        sys.stderr.write(f"read {path}: {e}\n")
        return [], offset
    for line in data.decode("utf-8", errors="replace").splitlines():
        s = line.strip()
        if not s or not s.startswith("{"):
            continue
        try:
            out.append(json.loads(s))
        except json.JSONDecodeError:
            continue
    return out, new_offset


def normalize(s: str) -> str:
    """Lowercase + alnum-only collapse — for fuzzy "is this an echo
    of what the assistant said" matching.  Whisper transcripts and
    LLM output won't be byte-identical but should overlap in
    normalized form when they cover the same words."""
    return "".join(c.lower() for c in s if c.isalnum())


def looks_like_echo(transcript: str, assistant_recent: list[str]) -> bool:
    t = normalize(transcript)
    if len(t) < 4:
        return False
    for sent in assistant_recent[-6:]:    # last ~6 sentences
        a = normalize(sent)
        if not a:
            continue
        # If the transcript is mostly contained in something the
        # assistant just said (or vice versa), call it echo.
        if t in a or (len(t) >= 12 and a in t):
            return True
        # ~70% common substring length on the shorter
        common = max(
            len(os.path.commonprefix([t, a])),
            len(os.path.commonprefix([t[::-1], a[::-1]])),
        )
        if common >= int(0.7 * min(len(t), len(a))):
            return True
    return False


def main() -> int:
    p = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p.add_argument("--attempts", type=int, default=5,
                    help="number of speak-now prompts (default 5)")
    p.add_argument("--window", type=float, default=6.0,
                    help="seconds to wait for a transcript per attempt")
    p.add_argument("--gap", type=float, default=2.0,
                    help="seconds between attempts (lets assistant resume TTS)")
    p.add_argument("--log-dir", default="/var/log/acva",
                    help="where acva-*.log live (default /var/log/acva)")
    args = p.parse_args()

    log = latest_log(args.log_dir)
    if not log:
        sys.stderr.write(
            f"no acva-*.log in {args.log_dir} — is acva running, or is\n"
            f"the log dir different? Pass --log-dir if so.\n"
        )
        return 2
    print(f"watching: {log}\n"
          f"acceptance: M6 gate 3 expects {args.attempts}/{args.attempts}.\n"
          f"speak a SHORT DISTINCTIVE phrase when prompted "
          f"(window: {args.window:.1f}s).\n"
          f"the script ignores transcripts that look like echo of "
          f"the assistant's recent speech.\n")

    # Snapshot offset — only react to log entries that arrive AFTER we start.
    offset = os.path.getsize(log)

    assistant_recent: list[str] = []
    passes = 0

    for i in range(1, args.attempts + 1):
        time.sleep(args.gap)
        print(f"[{i}/{args.attempts}] >>> SPEAK NOW <<<", flush=True)
        deadline = time.monotonic() + args.window
        captured: str | None = None

        while time.monotonic() < deadline:
            time.sleep(0.25)
            entries, offset = tail_since(log, offset)
            for e in entries:
                if e.get("event") == "llm_sentence":
                    txt = e.get("text", "")
                    if txt:
                        assistant_recent.append(txt)
                elif e.get("event") == "final_transcript":
                    text = e.get("text", "").strip()
                    if not text:
                        continue
                    if looks_like_echo(text, assistant_recent):
                        print(f"    skip (echo of assistant): '{text}'")
                        continue
                    captured = text
                    break
            if captured:
                break

        if captured:
            print(f"    PASS — transcript: '{captured}'")
            passes += 1
        else:
            print(f"    no clean transcript in {args.window:.1f}s window")

    print()
    print(f"barge-in audibility: {passes}/{args.attempts}")
    if passes == args.attempts:
        print(f"RESULT: PASS — M6 gate 3 met")
        return 0
    print(f"RESULT: FAIL — M6 gate 3 expects {args.attempts}/{args.attempts}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
