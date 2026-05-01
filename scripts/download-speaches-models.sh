#!/usr/bin/env bash
# download-speaches-models.sh — install Speaches' STT and TTS models.
#
# Speaches owns its own model installation: POST /v1/models/{id}
# triggers a HuggingFace download into the container's HF cache (which
# we bind-mount under ${XDG_DATA_HOME}/acva/models/speaches/). The
# endpoint is idempotent — 200 if already cached, 201 on first
# download.
#
# This script is the equivalent of `scripts/download-silero-vad.sh`
# for Speaches: succeeds env-var-free on a clean dev machine. It
# requires the Speaches container to already be reachable on
# ${ACVA_SPEACHES_URL:-http://127.0.0.1:8090} — bring it up first
# with `docker compose up -d speaches` from packaging/compose/.
#
# The model list is the smoke baseline for M4B; expand as the project
# adds languages.
set -euo pipefail

URL="${ACVA_SPEACHES_URL:-http://127.0.0.1:8090}"

# Wait up to 60 s for /health, since first start downloads runtime
# assets and can be slow.
echo "Waiting for Speaches at ${URL} ..."
deadline=$(( $(date +%s) + 60 ))
until curl -fsS "${URL}/health" >/dev/null 2>&1; do
    if (( $(date +%s) > deadline )); then
        echo "speaches: /health unreachable at ${URL} after 60s — is the container up?" >&2
        echo "  cd packaging/compose && docker compose up -d speaches" >&2
        exit 1
    fi
    sleep 2
done
echo "  /health 200 OK"

# Models to install. STT model is a single multilingual large model;
# TTS list mirrors the per-language voices configured in
# config/default.yaml. Add rows as new languages come online.
#
# Format: one model id per line; blank lines and lines starting with
# '#' are skipped.
MODELS=$(cat <<'EOF'
# STT — multilingual faster-whisper, ~1.5 GB on disk.
Systran/faster-whisper-large-v3
# TTS — Piper voices, one per language we support.
speaches-ai/piper-en_US-amy-medium
EOF
)

while IFS= read -r model; do
    [[ -z "$model" || "$model" =~ ^[[:space:]]*# ]] && continue
    # Encode the slash in the path. Speaches accepts `owner/name`
    # verbatim in the URL path with the slash unescaped, but quote
    # for safety.
    encoded=$(printf '%s' "$model" | sed 's| |%20|g')

    # Capture HTTP status and small body. -w prints status; -o
    # captures body so we can show it on error.
    body=$(mktemp)
    status=$(curl -sS -o "$body" -w '%{http_code}' \
                  -X POST "${URL}/v1/models/${encoded}" || true)
    case "$status" in
        200)
            echo "✓ ${model} (already cached)"
            ;;
        201)
            echo "↓ ${model} (downloaded)"
            ;;
        *)
            echo "✗ ${model}: HTTP ${status}" >&2
            head -c 400 "$body" >&2 || true
            echo >&2
            rm -f "$body"
            exit 1
            ;;
    esac
    rm -f "$body"
done <<< "$MODELS"

echo
echo "Installed models reported by Speaches:"
curl -fsS "${URL}/v1/models" \
  | python3 -c 'import json, sys; [print("  -", m["id"], "/", m.get("task","?")) for m in json.load(sys.stdin)["data"]]'
