#!/usr/bin/env bash
# build-times.sh — print per-TU compile times from the last ninja build.
#
# Reads `_build/<preset>/.ninja_log`, dumps a sorted table of object
# files by wall-clock build time. ninja stamps every output with start
# and end ms (relative to the build's start), so the delta is the
# wall-clock cost of compiling that TU — including waits for the
# scheduler, but at the granularity of a real build that's close to
# what the user perceives.
#
# Usage:
#   ./scripts/build-times.sh [preset]    # default preset: dev
#   ./scripts/build-times.sh dev 30      # top 30 instead of top 20
#
# For deeper per-TU breakdown (which include / template / instantiation
# cost what), rebuild with `-DACVA_TIME_TRACE=ON` and open the per-file
# JSONs in chrome://tracing or speedscope.

set -euo pipefail

preset="${1:-dev}"
top="${2:-20}"

log="_build/${preset}/.ninja_log"

if [[ ! -f "$log" ]]; then
    echo "build-times: no ninja log at $log — run ./build.sh ${preset} first" >&2
    exit 1
fi

# .ninja_log columns: start_ms end_ms restat_mtime command_hash output
# Skip the header line. Filter to .o outputs only (skip linked artifacts
# and other non-compile steps). Per-output entries are append-only and
# the LAST entry for an output is the most recent build — collapse to
# the latest using awk's last-wins semantics.
awk '
    NR > 1 && $4 ~ /\.o$/ {
        ms = $2 - $1
        last_ms[$4] = ms
        last_path[$4] = $4
    }
    END {
        for (k in last_ms) {
            printf "%6d ms  %s\n", last_ms[k], last_path[k]
        }
    }
' "$log" | sort -rn | head -"$top"

echo
total=$(awk 'NR > 1 && $4 ~ /\.o$/ {
    last_ms[$4] = $2 - $1
}
END {
    s = 0
    for (k in last_ms) s += last_ms[k]
    printf "%d", s
}' "$log")
count=$(awk 'NR > 1 && $4 ~ /\.o$/ { seen[$4] = 1 } END { n = 0; for (k in seen) ++n; print n }' "$log")

echo "  totals: ${count} TUs, $((total / 1000)).$(printf '%03d' $((total % 1000))) s sequential, "\
"avg $((total / count)) ms/TU"
