#!/usr/bin/env bash
# scripts/soak-vad-falsestarts.sh — M6B Gate 1 acceptance soak.
#
# Runs `acva --stdin` for --duration seconds with continuous TTS
# playback (prompts fed via a fifo), then reads
# voice_vad_false_starts_total from /metrics and reports a PASS/FAIL
# against the M6 acceptance threshold of < 1/min.
#
# Pre-flight: acva must be built (`./build.sh dev`) and the Speaches
# + llama containers must be up (`cd packaging/compose && docker
# compose up -d`).  This script does NOT bring them up — it would
# either race the health probe or duplicate the orchestrator logic.
#
# Usage:
#   ./scripts/soak-vad-falsestarts.sh                    # 30-min run
#   ./scripts/soak-vad-falsestarts.sh --quick            # 5-min sanity
#   ./scripts/soak-vad-falsestarts.sh --duration 3600    # 1-hour soak

set -euo pipefail

DURATION=1800           # seconds — 30 min
PROMPT_INTERVAL=60      # seconds between prompts
THRESHOLD=1.0           # PASS if false_starts/min < THRESHOLD
ACVA=./_build/dev/acva
METRICS_URL=http://127.0.0.1:9876/metrics

usage() {
    sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//;s/^#$//'
    exit "${1:-0}"
}

while [[ $# -gt 0 ]]; do
    case $1 in
        --duration)  DURATION=$2; shift 2;;
        --interval)  PROMPT_INTERVAL=$2; shift 2;;
        --threshold) THRESHOLD=$2; shift 2;;
        --binary)    ACVA=$2; shift 2;;
        --metrics)   METRICS_URL=$2; shift 2;;
        --quick)     DURATION=300; shift;;
        -h|--help)   usage 0;;
        *) echo "unknown arg: $1" >&2; usage 1;;
    esac
done

if [[ ! -x "$ACVA" ]]; then
    echo "fatal: $ACVA is not executable — run ./build.sh dev first" >&2
    exit 2
fi

# Prompts cycle through this list.  Each one should produce a multi-second
# TTS response so the speaker is busy while we count VAD false-starts.
PROMPTS=(
    "Tell me a short story about a curious cat."
    "Explain the difference between TCP and UDP in a few sentences."
    "What is the speed of light, and how was it first measured?"
    "Describe how a household refrigerator works."
    "Give me three uses for baking soda outside of cooking."
    "What's the difference between weather and climate?"
    "Tell me a fun fact about octopuses."
    "How does a transistor work, briefly?"
    "What are the main causes of ocean tides?"
    "Recite the first stanza of any well-known poem."
)

FIFO=$(mktemp -u /tmp/acva-soak.XXXXXX.fifo)
mkfifo "$FIFO"

cleanup() {
    if [[ -n "${WRITE_FD_OPEN:-}" ]]; then exec 3>&-; fi
    if [[ -n "${ACVA_PID:-}" ]] && kill -0 "$ACVA_PID" 2>/dev/null; then
        kill -TERM "$ACVA_PID" 2>/dev/null || true
        # Give acva up to 10 s to exit cleanly before forcing.
        for _ in $(seq 1 20); do
            kill -0 "$ACVA_PID" 2>/dev/null || break
            sleep 0.5
        done
        kill -KILL "$ACVA_PID" 2>/dev/null || true
        wait "$ACVA_PID" 2>/dev/null || true
    fi
    rm -f "$FIFO"
}
trap cleanup EXIT INT TERM

# Spawn acva reading the fifo on stdin
"$ACVA" --stdin < "$FIFO" >/tmp/acva-soak.out 2>&1 &
ACVA_PID=$!

# Open the fifo for writing in this shell so the read side blocks
# until we send a line, but the producer never sees EOF until cleanup.
exec 3>"$FIFO"
WRITE_FD_OPEN=1

# Wait for /metrics to come up (acva needs to start the HTTP server).
echo -n "waiting for acva HTTP server..."
for i in $(seq 1 60); do
    if curl -s -f -o /dev/null --max-time 1 "$METRICS_URL"; then
        echo " up after ${i}s"
        break
    fi
    if ! kill -0 "$ACVA_PID" 2>/dev/null; then
        echo
        echo "fatal: acva exited early.  Tail of /tmp/acva-soak.out:" >&2
        tail -30 /tmp/acva-soak.out >&2 || true
        exit 3
    fi
    sleep 1
done
if ! curl -s -f -o /dev/null --max-time 1 "$METRICS_URL"; then
    echo
    echo "fatal: acva HTTP server didn't come up within 60 s" >&2
    exit 3
fi

read_metric() {
    curl -s --max-time 3 "$METRICS_URL" \
        | awk -v name="$1" '$1 == name { print $2; exit }'
}

START_TS=$(date +%s)
START_FS=$(read_metric voice_vad_false_starts_total)
START_FS=${START_FS:-0}

echo "soak: duration=${DURATION}s interval=${PROMPT_INTERVAL}s threshold=${THRESHOLD}/min"
echo "baseline: voice_vad_false_starts_total=${START_FS}"
echo

PROMPT_IDX=0
NEXT_PROMPT=$(date +%s)
END=$(($(date +%s) + DURATION))

while [[ $(date +%s) -lt $END ]]; do
    if ! kill -0 "$ACVA_PID" 2>/dev/null; then
        echo "fatal: acva exited mid-soak — tail of /tmp/acva-soak.out:" >&2
        tail -30 /tmp/acva-soak.out >&2 || true
        exit 3
    fi
    NOW=$(date +%s)
    if [[ $NOW -ge $NEXT_PROMPT ]]; then
        PROMPT="${PROMPTS[$((PROMPT_IDX % ${#PROMPTS[@]}))]}"
        echo "$PROMPT" >&3
        PROMPT_IDX=$((PROMPT_IDX + 1))
        NEXT_PROMPT=$((NEXT_PROMPT + PROMPT_INTERVAL))
        REMAINING=$((END - NOW))
        FS_NOW=$(read_metric voice_vad_false_starts_total)
        FS_NOW=${FS_NOW:-0}
        printf "[%s] prompt %d sent (false_starts so far=%s, %ds remaining): %s\n" \
            "$(date +%T)" "$PROMPT_IDX" "$FS_NOW" "$REMAINING" "$PROMPT"
    fi
    sleep 5
done

END_TS=$(date +%s)
END_FS=$(read_metric voice_vad_false_starts_total)
END_FS=${END_FS:-0}

ELAPSED_MIN=$(awk -v a="$START_TS" -v b="$END_TS" 'BEGIN { printf "%.2f", (b-a)/60 }')
DELTA_FS=$(awk -v a="$START_FS" -v b="$END_FS" 'BEGIN { print b-a }')
RATE=$(awk -v d="$DELTA_FS" -v m="$ELAPSED_MIN" 'BEGIN {
    if (m+0 == 0) print "nan"; else printf "%.3f", d/m
}')

echo
echo "----- soak complete -----"
echo "elapsed:      ${ELAPSED_MIN} min"
echo "prompts sent: ${PROMPT_IDX}"
echo "false_starts: ${DELTA_FS} (rate = ${RATE}/min, threshold = ${THRESHOLD}/min)"

if awk -v r="$RATE" -v t="$THRESHOLD" 'BEGIN { exit !(r+0 < t+0) }'; then
    echo "RESULT: PASS — M6 gate 1 met (rate ${RATE} < ${THRESHOLD}/min)"
    exit 0
else
    echo "RESULT: FAIL — M6 gate 1 not met (rate ${RATE} >= ${THRESHOLD}/min)"
    echo "tail of /tmp/acva-soak.out:" >&2
    tail -20 /tmp/acva-soak.out >&2 || true
    exit 1
fi
