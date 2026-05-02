#!/usr/bin/env bash
# download-llm.sh — fetch LLM GGUF weights for `llama-server`.
#
# llama.cpp expects model files on disk before the server starts, so
# this script is plain curl into the host directory the Compose
# `llama` service bind-mounts as `/models`. Default destination:
# ${ACVA_MODELS_DIR:-${HOME}/.local/share/acva/models}.
#
# Layout: alias → entry parsed from the MODELS table. Aliases include
# the project default (`qwen2.5-7b`) + curated alternatives. With no
# args this script downloads the default; pass aliases to install
# alternatives instead. Each entry is (filename | size_bytes | url
# | one-line-purpose); when a multipart GGUF is needed, list each
# part as its own row with the same alias prefix (e.g. `qwen2.5-7b/1`).
#
# **Switching the active model:** set `ACVA_LLM_MODEL=<filename>`
# (in your shell or in `packaging/compose/.env`) and recreate llama:
#   ACVA_LLM_MODEL=socratic-tutor-v2-q4_k_m.gguf \
#     docker compose -f packaging/compose/docker-compose.yml up -d --force-recreate llama
# The compose default stays Qwen2.5-7B; flipping the env var is the
# only thing needed to switch.
#
# **VRAM budget reminder (RTX 4060 8 GB):**
#   llama-7B/8B Q4_K_M  ~4.5–5.0 GB
# + faster-whisper-large-v3-turbo ~1.6 GB
# + Kokoro/Piper warm ~0.5 GB
# = ~7.0 GB. Don't pull Q5/Q6 quants without evicting whisper/TTS.
#
# Idempotent (skips files matching the listed size). Resumable
# (curl --continue-at -).

set -euo pipefail

MODELS_ROOT="${ACVA_MODELS_DIR:-${HOME}/.local/share/acva/models}"
DEST="${MODELS_ROOT}/llama.cpp"
mkdir -p "$DEST"

# Migration: GGUFs used to live directly under MODELS_ROOT before the
# per-engine subfolder layout. If any legacy files are still there,
# move them into ./llama.cpp/. Same filesystem → instantaneous rename.
shopt -s nullglob
for legacy in "${MODELS_ROOT}"/*.gguf; do
    [[ -f "$legacy" ]] || continue
    fname=$(basename "$legacy")
    target="${DEST}/${fname}"
    if [[ -e "$target" ]]; then
        echo "↻ migration: ${fname} already in llama.cpp/, removing legacy copy at ${legacy}"
        rm -f "$legacy"
    else
        echo "↻ migration: moving ${fname} → llama.cpp/"
        mv "$legacy" "$target"
    fi
done
shopt -u nullglob

# Each row: alias <TAB> filename <TAB> size_bytes <TAB> url <TAB> purpose.
# Sharded GGUFs use multiple rows with the same alias.
read -r -d '' MODELS <<'EOF' || true
qwen2.5-7b	Qwen2.5-7B-Instruct-Q4_K_M.gguf	4683074240	https://huggingface.co/bartowski/Qwen2.5-7B-Instruct-GGUF/resolve/main/Qwen2.5-7B-Instruct-Q4_K_M.gguf	[default] Qwen2.5-7B-Instruct Q4_K_M (bartowski single-file, imatrix-calibrated)
socratic	socratic-tutor-v2-q4_k_m.gguf	4683073504	https://huggingface.co/RuudFontys/socratic-tutor-qwen2.5/resolve/main/socratic-tutor-v2-q4_k_m.gguf	Qwen2.5-7B-Instruct + Socratic-tutor LoRA (350 dialogues, 3 epochs)
doctor	DoctorAgent-RL.Q4_K_M.gguf	4683073984	https://huggingface.co/mradermacher/DoctorAgent-RL-GGUF/resolve/main/DoctorAgent-RL.Q4_K_M.gguf	Qwen2-8B + medical-RL fine-tune (mradermacher quant)
openbuddy-zephyr	openbuddy-zephyr-7b-v14.1-Mistral-7B-Instruct-v0.1.Q4_K_M.gguf	4368439488	https://huggingface.co/MaziyarPanahi/openbuddy-zephyr-7b-v14.1-Mistral-7B-Instruct-v0.1-GGUF/resolve/main/openbuddy-zephyr-7b-v14.1-Mistral-7B-Instruct-v0.1.Q4_K_M.gguf	OpenBuddy v14.1 multilingual chat over Zephyr/Mistral-7B (research/non-commercial license)
openbuddy-mistral	openbuddy-mistral-7b-v13.1-Mistral-7B-Instruct-v0.1.Q4_K_M.gguf	4368439488	https://huggingface.co/MaziyarPanahi/openbuddy-mistral-7b-v13.1-Mistral-7B-Instruct-v0.1-GGUF/resolve/main/openbuddy-mistral-7b-v13.1-Mistral-7B-Instruct-v0.1.Q4_K_M.gguf	OpenBuddy v13.1 multilingual chat over Mistral-7B-Instruct (research/non-commercial license)
outlier-lite	Outlier-Lite-7B-Q4_K_M.gguf	4683074080	https://huggingface.co/Outlier-Ai/Outlier-Lite-7B-GGUF/resolve/main/Outlier-Lite-7B-Q4_K_M.gguf	Outlier-Lite-7B chat fine-tune over Qwen2.5 base (Apache-2.0)
dialog	qwen3-stage1.q4_k_m.gguf	5027783520	https://huggingface.co/milwright/qwen-8b-dialog-v1/resolve/main/qwen3-stage1.q4_k_m.gguf	Qwen3-8B stage-1 dialogue tune (single Q4_K_M shipped; license unstated)
EOF

# Repos investigated and rejected from this manifest:
#   JeffGreen311/eve-qwen3-8b-consciousness          — safetensors only (4-shard 15.2 GB), no GGUF
#   JeffGreen311/eve-qwen3-8b-consciousness-liberated — F16 GGUF only (15.2 GB), exceeds VRAM budget
#   JeffGreen311/eve-qwen2.5-3b-consciousness-soul   — F16 GGUF 6.18 GB (no Q4 shipped); too tight
#                                                       alongside whisper-turbo without
#                                                       evicting Kokoro/Piper. Local
#                                                       `llama-quantize ... Q4_K_M` would
#                                                       fix it; out of scope for this script.
#   Hypersniper/The_Philosopher_Zephyr_7B            — safetensors only (3-shard 14.5 GB), no GGUF
# If you want any of these, look for a community Q4_K_M re-upload (TheBloke /
# mradermacher mirrors are the usual place) and add a row above.

usage() {
    cat <<EOF
download-llm.sh — fetch LLM GGUF weights

Usage:
  $0 [alias [alias …]]    install one or more aliases
  $0                       install the default (qwen2.5-7b)
  $0 -h | --help

Aliases:
EOF
    awk -F'\t' '
        $1 != prev { if (prev) printf "\n"; printf "  %-12s %s\n", $1, $5; prev=$1; next }
        { printf "  %-12s %s\n", "", $5 }
    ' <<< "$MODELS"
    cat <<EOF

Destination: $DEST
After install, set ACVA_LLM_MODEL=<filename> and recreate the llama service.
EOF
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
    usage
    exit 0
fi

if (( $# > 0 )); then
    targets=("$@")
else
    targets=("qwen2.5-7b")
fi

for target in "${targets[@]}"; do
    if ! awk -F'\t' -v a="$target" '$1 == a { found=1 } END { exit !found }' <<< "$MODELS"; then
        echo "unknown alias: $target" >&2
        echo >&2
        usage >&2
        exit 1
    fi

    while IFS=$'\t' read -r alias fname size url purpose; do
        [[ -z "$alias" || "$alias" != "$target" ]] && continue
        out="${DEST}/${fname}"

        if [[ -f "$out" ]]; then
            actual=$(stat -c%s "$out")
            if [[ "$actual" == "$size" ]]; then
                printf '✓ %-12s %s — already present, %s bytes\n' "$alias" "$fname" "$actual"
                continue
            fi
            printf '↻ %-12s %s — partial (%s of %s); resuming\n' \
                "$alias" "$fname" "$actual" "$size"
        else
            printf '↓ %-12s %s\n   %s\n' "$alias" "$purpose" "$url"
        fi

        curl -L --fail-with-body -C - --retry 5 --retry-delay 5 \
             --connect-timeout 30 -o "$out" "$url"

        actual=$(stat -c%s "$out")
        if [[ "$actual" != "$size" ]]; then
            printf '✗ %s: downloaded %s bytes, expected %s\n' \
                "$fname" "$actual" "$size" >&2
            exit 1
        fi
        printf '✓ %-12s %s — %s bytes\n' "$alias" "$fname" "$actual"
    done <<< "$MODELS"
done

echo
echo "GGUFs in $DEST:"
ls -lh "$DEST"/*.gguf 2>/dev/null | awk '{ printf "  %s %s\n", $5, $9 }'
