#!/usr/bin/env bash
# download-llm-models.sh — fetch alternative LLM GGUF files for
# `llama-server`. The Compose `llama` service bind-mounts
# ${ACVA_MODELS_DIR:-${HOME}/.local/share/acva/models}/ into /models;
# this script downloads GGUFs into that directory so they're visible
# to the container.
#
# Why this is separate from `download-speaches-models.sh`:
# Speaches owns its own model installation (POST /v1/models/{id}
# fetches HuggingFace into the container's HF cache). llama.cpp has
# no such API — it expects a file on disk before the server starts.
# So this script is plain curl; idempotent (skips when the local
# file already matches the listed size).
#
# **Switching the active model:** set `ACVA_LLM_MODEL=<filename>`
# (in your shell or in `packaging/compose/.env`) and re-up Compose:
#
#   ACVA_LLM_MODEL=socratic-tutor-v2-q4_k_m.gguf \
#       docker compose -f packaging/compose/docker-compose.yml up -d --force-recreate llama
#
# The default in the compose file stays
# `qwen2.5-7b-instruct-q4_k_m-00001-of-00002.gguf` so untouched
# setups behave as before.
#
# **VRAM budget reminder (RTX 4060 8 GB):**
#   llama-7B Q4_K_M  ~4.5 GB
# + faster-whisper-large-v3-turbo ~1.6 GB
# + Kokoro/Piper warm ~0.5 GB
# + buffers ~0.3 GB
# = ~7.0 GB — leaves ~1 GB headroom. Don't pull Q5/Q6 quants here
# unless you've also evicted Whisper/TTS.

set -euo pipefail

DEST="${ACVA_MODELS_DIR:-${HOME}/.local/share/acva/models}"
mkdir -p "$DEST"

# Alias       => "filename|expected_size_bytes|url|one-line-purpose"
#
# Aliases are what the user types as the script's first arg
# (`./download-llm-models.sh socratic`). Sizes lock in idempotency:
# if the on-disk file matches both the filename and the byte count,
# we don't re-download. Actual model evaluation lives in `plans/...`
# and `RuudFontys/.../README.md`; this script just fetches bytes.
declare -A MODELS=(
    [socratic]="socratic-tutor-v2-q4_k_m.gguf|4683073504|https://huggingface.co/RuudFontys/socratic-tutor-qwen2.5/resolve/main/socratic-tutor-v2-q4_k_m.gguf|Qwen2.5-7B-Instruct + Socratic-tutor LoRA (350 dialogues, 3 epochs)"
    [doctor]="DoctorAgent-RL.Q4_K_M.gguf|4683073984|https://huggingface.co/mradermacher/DoctorAgent-RL-GGUF/resolve/main/DoctorAgent-RL.Q4_K_M.gguf|Qwen2-8B + medical-RL fine-tune (mradermacher quant)"
)

usage() {
    cat <<EOF
download-llm-models.sh — fetch alternative LLM GGUF files

Usage:
  $0 [alias [alias …]]

If no alias is given, all listed aliases are downloaded.

Aliases:
EOF
    for alias in "${!MODELS[@]}"; do
        IFS='|' read -r fname _size _url purpose <<< "${MODELS[$alias]}"
        printf "  %-10s %s\n             %s\n" "$alias" "$fname" "$purpose"
    done
    cat <<EOF

Destination: $DEST
Switching: set ACVA_LLM_MODEL=<filename> in the compose env and recreate the llama service.
EOF
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
    usage
    exit 0
fi

if (( $# > 0 )); then
    targets=("$@")
else
    targets=("${!MODELS[@]}")
fi

for alias in "${targets[@]}"; do
    if [[ -z "${MODELS[$alias]:-}" ]]; then
        echo "unknown alias: $alias" >&2
        echo >&2
        usage >&2
        exit 1
    fi
    IFS='|' read -r fname size url purpose <<< "${MODELS[$alias]}"
    out="${DEST}/${fname}"

    if [[ -f "$out" ]]; then
        actual=$(stat -c%s "$out")
        if [[ "$actual" == "$size" ]]; then
            echo "✓ ${alias} (${fname}) — already present, ${actual} bytes"
            continue
        fi
        echo "⚠ ${alias}: stale local file (${actual} != ${size} bytes); re-downloading"
        rm -f "$out"
    fi

    echo "↓ ${alias}: ${purpose}"
    echo "   ${url}"
    echo "   → ${out}"
    # -L follow redirects; -C - resume; --fail-with-body so HTTP
    # errors propagate instead of writing the error page to disk.
    curl -L --fail-with-body -C - -o "$out.part" "$url"
    mv "$out.part" "$out"

    actual=$(stat -c%s "$out")
    if [[ "$actual" != "$size" ]]; then
        echo "✗ ${alias}: downloaded ${actual} bytes, expected ${size}" >&2
        exit 1
    fi
    echo "✓ ${alias} (${fname}) — ${actual} bytes"
done

echo
echo "Models in ${DEST}:"
ls -lh "$DEST"/*.gguf 2>/dev/null | awk '{ printf "  %s %s\n", $5, $9 }'
