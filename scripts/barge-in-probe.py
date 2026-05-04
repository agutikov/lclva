#!/usr/bin/env python3
"""M6B Gate 3 — barge-in audibility probe.

Self-contained: starts acva, feeds it a long-response prompt so the
assistant is speaking continuously, runs N speak-now windows, then
shuts acva down.  The user only has to read the on-screen
instructions and speak when prompted.

For each window, looks for a `final_transcript` event in the log.
Rejects transcripts that fuzz-match the assistant's recent
`llm_sentence` (echo).  PASS criterion: N/N clean transcripts.

Usage:
    ./scripts/barge-in-probe.py                  # 5 attempts, ru
    ./scripts/barge-in-probe.py --attempts 3
    ./scripts/barge-in-probe.py --lang en --prompt 'Tell me a long story.'
    ./scripts/barge-in-probe.py --binary _build/debug/acva
"""

from __future__ import annotations

import argparse
import glob
import json
import os
import signal
import subprocess
import sys
import time
import urllib.error
import urllib.request

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))


# ---------------------------------------------------------------------------
# log tailing + echo discrimination (unchanged from prior version)
# ---------------------------------------------------------------------------

def tail_since(path: str, offset: int) -> tuple[list[dict], int]:
    """Read everything in `path` from `offset` onwards. Returns
    (parsed JSON entries, new offset). Skips non-JSON lines silently
    — acva log files mix JSON events with occasional ALSA stderr."""
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
    return "".join(c.lower() for c in s if c.isalnum())


def looks_like_echo(transcript: str, assistant_recent: list[str]) -> bool:
    t = normalize(transcript)
    if len(t) < 4:
        return False
    for sent in assistant_recent[-6:]:
        a = normalize(sent)
        if not a:
            continue
        if t in a or (len(t) >= 12 and a in t):
            return True
        common = max(
            len(os.path.commonprefix([t, a])),
            len(os.path.commonprefix([t[::-1], a[::-1]])),
        )
        if common >= int(0.7 * min(len(t), len(a))):
            return True
    return False


# ---------------------------------------------------------------------------
# acva lifecycle
# ---------------------------------------------------------------------------

# Default Russian prompt picked to produce ≥ 10 long sentences of
# narration (no list/enumeration which would chunk into short bursts).
DEFAULT_PROMPTS = {
    "ru": "Расскажи длинную интересную историю про путешествие на Марс. "
          "Минимум десять предложений, с описаниями пейзажей и эмоциями героев.",
    "en": "Tell me a long, interesting story about a journey to Mars. "
          "At least ten sentences, with descriptions of the landscape and "
          "the characters' emotions.",
}

# Phrase suggestions shown to the human. Picked so the echo-guard
# won't eat them — none of these tokens would naturally appear in the
# Mars-story prompt response.
SUGGESTIONS = {
    "ru": ['"красный круг"', '"проверка раз два три"', '"стоп пожалуйста"'],
    "en": ['"red square"', '"testing one two three"', '"please stop"'],
}


def find_binary(explicit: str | None) -> str:
    if explicit:
        path = explicit if os.path.isabs(explicit) else os.path.join(REPO_ROOT, explicit)
        if not os.access(path, os.X_OK):
            sys.stderr.write(f"barge-in-probe: --binary {path} is not executable\n")
            sys.exit(2)
        return path
    for rel in ("_build/dev/acva", "_build/debug/acva", "_build/release/acva"):
        path = os.path.join(REPO_ROOT, rel)
        if os.access(path, os.X_OK):
            return path
    sys.stderr.write(
        "barge-in-probe: no acva binary found under _build/. "
        "Run `./build.sh dev` first or pass --binary.\n"
    )
    sys.exit(2)


def wait_for_metrics(url: str, timeout: float, proc: subprocess.Popen) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            return False  # acva exited early
        try:
            with urllib.request.urlopen(url, timeout=1) as r:
                if r.status == 200:
                    return True
        except (urllib.error.URLError, ConnectionError, TimeoutError):
            pass
        time.sleep(0.5)
    return False


def latest_log(log_dir: str, after_mtime: float) -> str | None:
    """Return the newest acva-*.log whose mtime is >= after_mtime."""
    candidates = []
    for path in glob.glob(os.path.join(log_dir, "acva-*.log")):
        try:
            mtime = os.path.getmtime(path)
        except OSError:
            continue
        if mtime >= after_mtime:
            candidates.append((mtime, path))
    if not candidates:
        return None
    candidates.sort()
    return candidates[-1][1]


def wait_for_tts(log_path: str, offset: int, timeout: float) -> tuple[int, list[str]]:
    """Block until the assistant has emitted at least one llm_sentence
    AND a tts_started event has fired (so we know speakers are
    actually playing, not just LLM streaming). Returns (new offset,
    accumulated llm_sentences)."""
    deadline = time.monotonic() + timeout
    sentences: list[str] = []
    saw_tts = False
    while time.monotonic() < deadline:
        time.sleep(0.25)
        entries, offset = tail_since(log_path, offset)
        for e in entries:
            ev = e.get("event")
            if ev == "llm_sentence":
                txt = e.get("text", "")
                if txt:
                    sentences.append(txt)
            elif ev == "tts_started":
                saw_tts = True
        if sentences and saw_tts:
            return offset, sentences
    return offset, sentences  # caller decides if this is fatal


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

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
    p.add_argument("--lang", default="ru",
                    help="--stdin-lang for acva (default ru). Also picks the "
                         "default prompt + speak-now phrase suggestions.")
    p.add_argument("--prompt",
                    help="text fed to acva on stdin to trigger the long TTS "
                         "response (defaults vary by --lang).")
    p.add_argument("--binary",
                    help="path to acva binary (default: auto-detect under "
                         "_build/{dev,debug,release}).")
    p.add_argument("--log-dir", default="/var/log/acva",
                    help="where acva-*.log live (default /var/log/acva)")
    p.add_argument("--startup-timeout", type=float, default=30.0,
                    help="max seconds to wait for acva HTTP server")
    p.add_argument("--tts-timeout", type=float, default=20.0,
                    help="max seconds to wait for first tts_started event")
    args = p.parse_args()

    binary = find_binary(args.binary)
    prompt = args.prompt or DEFAULT_PROMPTS.get(args.lang)
    if not prompt:
        sys.stderr.write(
            f"barge-in-probe: no default prompt for --lang {args.lang}; "
            f"pass --prompt explicitly.\n"
        )
        return 2

    suggestions = SUGGESTIONS.get(args.lang, SUGGESTIONS["en"])

    print("M6B gate 3 — barge-in audibility probe")
    print()
    print("HOW THIS GOES:")
    print("  - acva will start and the assistant will begin speaking a long story.")
    print(f"  - You'll be prompted {args.attempts} times to speak.")
    print("  - When you see  >>> SPEAK NOW <<<  say a SHORT, DISTINCTIVE phrase.")
    print(f"    Try one of: {', '.join(suggestions)}.")
    print("  - The phrase must NOT be something the assistant is saying.")
    print(f"  - Pass = clean transcript fires within {args.window:.0f}s and isn't echo.")
    print()
    print("starting acva...")

    # Track only logs created from this point onwards so we don't
    # latch onto a stale file from a previous run.
    start_mtime = time.time()

    proc = subprocess.Popen(
        [binary, "--stdin", "--stdin-lang", args.lang],
        cwd=REPO_ROOT,
        stdin=subprocess.PIPE,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        text=True,
    )

    def cleanup() -> None:
        if proc.poll() is None:
            try:
                proc.send_signal(signal.SIGTERM)
                proc.wait(timeout=10)
            except (ProcessLookupError, subprocess.TimeoutExpired):
                proc.kill()
                proc.wait()

    try:
        if not wait_for_metrics("http://127.0.0.1:9876/metrics",
                                  args.startup_timeout, proc):
            sys.stderr.write(
                f"barge-in-probe: acva did not bring up /metrics within "
                f"{args.startup_timeout:.0f}s. Check that the compose "
                f"backends (llama + speaches) are up.\n"
            )
            return 3

        # Find the log file acva just opened.  Logs are named with a
        # second-resolution timestamp; allow ~2 s skew.
        log_path = latest_log(args.log_dir, start_mtime - 2)
        if not log_path:
            sys.stderr.write(
                f"barge-in-probe: no fresh acva log appeared in "
                f"{args.log_dir}.\n"
            )
            return 3

        offset = 0  # start of the freshly-created log

        print("seeding prompt to trigger continuous TTS...")
        assert proc.stdin is not None
        proc.stdin.write(prompt + "\n")
        proc.stdin.flush()

        offset, sentences = wait_for_tts(log_path, offset, args.tts_timeout)
        if not sentences:
            sys.stderr.write(
                "barge-in-probe: assistant never started speaking. "
                "Check llm + tts service health.\n"
            )
            return 4

        print("assistant is speaking. starting probe windows.\n")

        assistant_recent = list(sentences)
        passes = 0

        for i in range(1, args.attempts + 1):
            time.sleep(args.gap)
            print(f"[{i}/{args.attempts}] >>> SPEAK NOW <<<", flush=True)
            deadline = time.monotonic() + args.window
            captured: str | None = None

            while time.monotonic() < deadline:
                time.sleep(0.25)
                entries, offset = tail_since(log_path, offset)
                for e in entries:
                    ev = e.get("event")
                    if ev == "llm_sentence":
                        txt = e.get("text", "")
                        if txt:
                            assistant_recent.append(txt)
                    elif ev == "final_transcript":
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
            print("RESULT: PASS — M6 gate 3 met")
            return 0
        print(f"RESULT: FAIL — M6 gate 3 expects {args.attempts}/{args.attempts}")
        return 1
    finally:
        cleanup()


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
