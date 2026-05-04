#!/usr/bin/env python3
# m7-bargein-bench.py — M7 acceptance benchmark for barge-in latency.
#
# Loops `acva demo bargein` N times against the live stack, parses
# `time_to_cancel_ms` and the cascade-fired markers from each run, and
# reports percentiles. Exit 0 only when the M7 acceptance gates are met:
#
#   §19.3:  P50 ≤ 200 ms,  P95 ≤ 400 ms
#
# What this DOES NOT cover: real-mic-driven barge-in (BargeInDetector +
# AEC gate). That's the manual 50-trial test in
# plans/milestones/m7_barge_in.md §5 — see Step 5 (deferred).
#
# Requires: docker stack (llama + speaches) up, models installed, and
# `_build/dev/acva` already compiled.
#
# Usage:
#   scripts/m7-bargein-bench.py            # N=20 (default), skip first
#   scripts/m7-bargein-bench.py --n 50
#   scripts/m7-bargein-bench.py --build    # cmake --build first
#   scripts/m7-bargein-bench.py --no-warm  # don't skip the first sample

from __future__ import annotations

import argparse
import os
import re
import statistics
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Optional


# -- ANSI colors (auto-disabled when not a TTY or NO_COLOR is set) ----

class _C:
    def __init__(self):
        self.on = (sys.stdout.isatty()
                   and "NO_COLOR" not in os.environ
                   and os.environ.get("TERM", "") != "dumb")
    def w(self, code, s):  return f"\033[{code}m{s}\033[0m" if self.on else s
    def b(self, s):        return self.w("1",    s)
    def dim(self, s):      return self.w("2",    s)
    def red(self, s):      return self.w("31",   s)
    def green(self, s):    return self.w("32",   s)
    def yellow(self, s):   return self.w("33",   s)
    def cyan(self, s):     return self.w("36",   s)
    def hi(self, s):       return self.w("1;37", s)
C = _C()
OK   = C.green("✓")
BAD  = C.red("✗")
WARN = C.yellow("⚠")
SKIP = C.dim("·")


# -- Demo output parsing ---------------------------------------------

# matches `time_to_cancel_ms=15.4 sentences_played=1 sentences_dropped=6 outcome=interrupted`
_DONE_LINE = re.compile(
    r"time_to_cancel_ms=(?P<lat>[\d.]+)\s+"
    r"sentences_played=(?P<played>\d+)\s+"
    r"sentences_dropped=(?P<dropped>\d+)\s+"
    r"outcome=(?P<outcome>\w+)"
)


@dataclass
class Run:
    iter:    int
    rc:      int
    lat_ms:  Optional[float]
    played:  Optional[int]
    dropped: Optional[int]
    outcome: Optional[str]
    raw:     str

    @property
    def cascade_fired(self) -> bool:
        return ((self.dropped or 0) > 0) or self.outcome == "interrupted"


def repo_root() -> Path:
    return Path(__file__).resolve().parent.parent


def run_demo_once(acva: Path, iter_n: int, timeout: float = 90.0) -> Run:
    """Run `acva demo bargein` once and parse its done-line."""
    try:
        r = subprocess.run(
            [str(acva), "demo", "bargein"],
            capture_output=True, text=True, timeout=timeout,
            cwd=str(repo_root()),
        )
        out = r.stdout + "\n" + r.stderr
        rc  = r.returncode
    except subprocess.TimeoutExpired as ex:
        return Run(iter_n, rc=124, lat_ms=None, played=None, dropped=None,
                    outcome="timeout", raw=str(ex))

    m = _DONE_LINE.search(out)
    if not m:
        return Run(iter_n, rc=rc, lat_ms=None, played=None, dropped=None,
                    outcome=None, raw=out)
    return Run(
        iter=iter_n, rc=rc,
        lat_ms=float(m.group("lat")),
        played=int(m.group("played")),
        dropped=int(m.group("dropped")),
        outcome=m.group("outcome"),
        raw=out,
    )


def percentile(xs: list[float], p: float) -> float:
    """Linear-interpolation percentile to match Prometheus' histogram_quantile
    naming. p in [0, 1]. Returns NaN for empty input."""
    if not xs:
        return float("nan")
    xs = sorted(xs)
    if len(xs) == 1:
        return xs[0]
    k = (len(xs) - 1) * p
    lo, hi = int(k), min(int(k) + 1, len(xs) - 1)
    return xs[lo] + (xs[hi] - xs[lo]) * (k - lo)


def main() -> int:
    p = argparse.ArgumentParser(
        description="M7 barge-in cancellation-cascade latency benchmark.")
    p.add_argument("--n", type=int, default=20,
                    help="Number of `demo bargein` invocations (default 20).")
    p.add_argument("--build", action="store_true",
                    help="Run `./build.sh` before measuring.")
    p.add_argument("--no-warm", action="store_true",
                    help="Don't drop the first sample (default drops it to "
                          "absorb llama cold-load).")
    p.add_argument("--timeout", type=float, default=90.0,
                    help="Per-invocation timeout in seconds (default 90).")
    args = p.parse_args()

    root = repo_root()
    acva = root / "_build" / "dev" / "acva"
    if args.build:
        print(f"{C.cyan('↻')} building…")
        subprocess.run(["./build.sh", "dev"], check=True, cwd=str(root))
    if not acva.is_file():
        sys.exit(f"{BAD} {acva} not found — build acva first (./build.sh dev)")

    # Pre-flight stack check: we need llama + speaches up. demo bargein
    # itself calls client.probe() and exits early on failure.
    print(f"{C.hi('M7 bargein bench')} {C.dim(f'(N={args.n}, target P50≤200ms P95≤400ms)')}")
    print(f"{C.dim('binary:')} {acva}")
    print()

    runs: list[Run] = []
    for i in range(1, args.n + 1):
        sys.stdout.write(f"  [{i:3d}/{args.n}] running… ")
        sys.stdout.flush()
        r = run_demo_once(acva, i, timeout=args.timeout)
        runs.append(r)
        if r.rc == 0 and r.cascade_fired and r.lat_ms is not None:
            sys.stdout.write(f"{OK} {C.b(f'{r.lat_ms:>6.1f} ms')} "
                              f"{C.dim(f'dropped={r.dropped} played={r.played}')}\n")
        elif r.rc == 124:
            sys.stdout.write(f"{BAD} {C.red('timeout')}\n")
        elif r.outcome == "completed":
            sys.stdout.write(f"{WARN} {C.yellow('LLM finished before inject')} "
                              f"{C.dim(f'(lat={r.lat_ms} dropped={r.dropped})')}\n")
        elif r.lat_ms is None:
            sys.stdout.write(f"{BAD} {C.red(f'parse fail (rc={r.rc})')}\n")
            # Tail of output for diagnostic
            tail = "\n".join(r.raw.splitlines()[-3:])
            sys.stdout.write(f"      {C.dim(tail)}\n")
        else:
            sys.stdout.write(f"{BAD} {C.red(f'rc={r.rc}')}\n")

    # ---- Filter samples ----
    skipped_warm = 0
    samples = runs[:]
    if not args.no_warm and len(samples) > 1:
        samples = samples[1:]
        skipped_warm = 1

    # Categorize each run into exactly one bucket. Latency is only
    # meaningful when audio was actively being silenced — i.e. either
    # the LLM was cancelled mid-stream, or the bridge had pending
    # sentences to drop (LLM finished naturally but TTS/playback was
    # still in flight).
    interrupted = [r for r in samples
                   if r.outcome == "interrupted"]
    late_inject = [r for r in samples
                   if r.outcome == "completed" and (r.dropped or 0) > 0]
    noop_race   = [r for r in samples
                   if r.outcome == "completed" and (r.dropped or 0) == 0]
    failures    = [r for r in samples
                   if r.rc not in (0, 124) and r.outcome not in ("completed", "interrupted")]
    timeouts    = [r for r in samples if r.rc == 124]

    lats = [r.lat_ms for r in (interrupted + late_inject) if r.lat_ms is not None]

    # ---- Report ----
    print()
    print(C.hi("Summary"))
    if skipped_warm:
        print(f"  {C.dim('warm-up sample skipped (use --no-warm to keep)')}")
    print(f"  {C.dim('total runs:'):<28s} {len(samples)}")
    print(f"  {C.dim('outcome=interrupted:'):<28s} "
          f"{C.green(str(len(interrupted))) if interrupted else SKIP}")
    if late_inject:
        print(f"  {C.dim('outcome=completed (drops>0):'):<28s} "
              f"{C.yellow(str(len(late_inject)))} "
              f"{C.dim('(LLM finished before cancel landed; bridge still cleared queue)')}")
    if noop_race:
        print(f"  {C.dim('outcome=completed (drops=0):'):<28s} "
              f"{C.yellow(str(len(noop_race)))} "
              f"{C.dim('(prompt too short — no audio was playing at inject)')}")
    if failures:
        print(f"  {C.dim('failures:'):<28s} {C.red(str(len(failures)))}")
    if timeouts:
        print(f"  {C.dim('timeouts:'):<28s} {C.red(str(len(timeouts)))}")

    if not lats:
        print(f"\n{BAD} {C.red('no real-interrupt samples — cannot compute latency')}")
        return 2

    p50  = percentile(lats, 0.50)
    p95  = percentile(lats, 0.95)
    print()
    print(C.hi("Latency (ms, real-interrupts only)"))
    print(f"  {C.dim('count:'):<28s} {len(lats)}")
    print(f"  {C.dim('min / max:'):<28s} {C.b(f'{min(lats):.1f}')} / {C.b(f'{max(lats):.1f}')}")
    print(f"  {C.dim('mean / stdev:'):<28s} "
          f"{C.b(f'{statistics.mean(lats):.1f}')}  /  "
          f"{C.b(f'{statistics.stdev(lats):.1f}' if len(lats) > 1 else '—')}")
    p50_ok = p50 <= 200.0
    p95_ok = p95 <= 400.0
    print(f"  {C.dim('P50:'):<28s} {C.b(f'{p50:.1f}')}  "
          f"{OK + ' ≤ 200' if p50_ok else BAD + ' > 200 (gate failed)'}")
    print(f"  {C.dim('P95:'):<28s} {C.b(f'{p95:.1f}')}  "
          f"{OK + ' ≤ 400' if p95_ok else BAD + ' > 400 (gate failed)'}")

    print()
    if p50_ok and p95_ok and not failures and not timeouts:
        print(f"{OK} {C.green('M7 acceptance gate §19.3 met.')}")
        return 0
    print(f"{BAD} {C.red('M7 acceptance gate §19.3 NOT met')} "
          f"{C.dim('— see numbers above.')}")
    return 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
