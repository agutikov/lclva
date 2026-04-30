#!/usr/bin/env bash
# fetch-assets.sh — download the default LLM weights, Whisper model, and
# Piper voice into the host paths the compose stack expects.
#
# Idempotent: re-runs skip files that already exist with the expected
# size. Resumable on partial downloads (curl --continue-at -).
#
# Override the destination roots with ACVA_MODELS_DIR / ACVA_VOICES_DIR
# (same env vars compose reads). Total disk footprint of the defaults
# below: ~5.2 GB.

set -euo pipefail

MODELS_DIR="${ACVA_MODELS_DIR:-${HOME}/.local/share/acva/models}"
VOICES_DIR="${ACVA_VOICES_DIR:-${HOME}/.local/share/acva/voices}"

mkdir -p "${MODELS_DIR}" "${VOICES_DIR}"

# Asset catalog: rows are TAB-separated (dest_dir, filename, expected_size, url).
# Sizes are taken from the upstream Content-Length and used to short-circuit
# already-complete downloads. They are NOT cryptographic verification — if you
# need that, add SHA256 entries and `sha256sum -c`.
read -r -d '' ASSETS <<'EOF' || true
LLM_SHARD_1	qwen2.5-7b-instruct-q4_k_m-00001-of-00002.gguf	3993201344	https://huggingface.co/Qwen/Qwen2.5-7B-Instruct-GGUF/resolve/main/qwen2.5-7b-instruct-q4_k_m-00001-of-00002.gguf
LLM_SHARD_2	qwen2.5-7b-instruct-q4_k_m-00002-of-00002.gguf	689872288	https://huggingface.co/Qwen/Qwen2.5-7B-Instruct-GGUF/resolve/main/qwen2.5-7b-instruct-q4_k_m-00002-of-00002.gguf
WHISPER	ggml-small.bin	487601967	https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-small.bin
PIPER_VOICE	en_US-amy-medium.onnx	63201294	https://huggingface.co/rhasspy/piper-voices/resolve/main/en/en_US/amy/medium/en_US-amy-medium.onnx
PIPER_CFG	en_US-amy-medium.onnx.json	4882	https://huggingface.co/rhasspy/piper-voices/resolve/main/en/en_US/amy/medium/en_US-amy-medium.onnx.json
EOF

target_dir() {
    case "$1" in
        LLM_*|WHISPER) echo "${MODELS_DIR}" ;;
        PIPER_*)       echo "${VOICES_DIR}" ;;
        *)             echo "unknown asset class: $1" >&2; return 1 ;;
    esac
}

human() {
    local b="$1"
    if   (( b > 1073741824 )); then printf '%.1f GiB' "$(echo "scale=2;$b/1073741824" | bc)"
    elif (( b > 1048576    )); then printf '%.1f MiB' "$(echo "scale=2;$b/1048576" | bc)"
    elif (( b > 1024       )); then printf '%.1f KiB' "$(echo "scale=2;$b/1024" | bc)"
    else printf '%d B' "$b"
    fi
}

while IFS=$'\t' read -r class name expected_size url; do
    [[ -z "$class" ]] && continue
    dir=$(target_dir "$class")
    dest="${dir}/${name}"

    if [[ -f "$dest" ]]; then
        actual=$(stat -c '%s' "$dest")
        if [[ "$actual" == "$expected_size" ]]; then
            printf '✓ %-50s already present (%s)\n' "$name" "$(human "$actual")"
            continue
        fi
        printf '↻ %-50s partial (%s of %s) — resuming\n' \
            "$name" "$(human "$actual")" "$(human "$expected_size")"
    else
        printf '↓ %-50s downloading %s\n' "$name" "$(human "$expected_size")"
    fi

    curl --fail --location --continue-at - \
         --retry 5 --retry-delay 5 \
         --connect-timeout 30 \
         --output "$dest" \
         "$url"

    actual=$(stat -c '%s' "$dest")
    if [[ "$actual" != "$expected_size" ]]; then
        printf '✗ %s size mismatch: got %s, expected %s\n' \
            "$name" "$(human "$actual")" "$(human "$expected_size")" >&2
        exit 1
    fi
done <<< "$ASSETS"

echo
echo "All assets present:"
echo "  models: ${MODELS_DIR}"
echo "  voices: ${VOICES_DIR}"
