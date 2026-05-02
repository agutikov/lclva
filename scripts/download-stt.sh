#!/usr/bin/env bash
# download-stt.sh — install Speaches' STT model(s).
#
# Speaches owns its own installation: POST /v1/models/{id} triggers
# a HuggingFace download into the container's HF cache (bind-mounted
# under ${XDG_DATA_HOME}/acva/models/speaches/). Idempotent — 200 if
# already cached, 201 on first download.
#
# Sibling type-organized scripts:
#   download-llm.sh — LLM GGUFs for llama-server
#   download-tts.sh — Speaches TTS voices
#   download-vad.sh — Silero VAD ONNX
#   download-assets.sh — umbrella that runs all four
#
# Requires Speaches reachable at ${ACVA_SPEACHES_URL:-http://127.0.0.1:8090}.
# Bring up the container first:
#   docker compose -f packaging/compose/docker-compose.yml up -d speaches

set -euo pipefail

URL="${ACVA_SPEACHES_URL:-http://127.0.0.1:8090}"

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

# STT — multilingual faster-whisper-large-v3-turbo (~1.6 GB float16).
# Smaller distilled decoder over the same large-v3 encoder; fits
# alongside llama-7B-Q4 on an 8 GB GPU. See config/default.yaml's
# stt.model comment for the rationale.
MODELS=$(cat <<'EOF'
deepdml/faster-whisper-large-v3-turbo-ct2
EOF
)

while IFS= read -r model; do
    [[ -z "$model" || "$model" =~ ^[[:space:]]*# ]] && continue
    encoded=$(printf '%s' "$model" | sed 's| |%20|g')
    body=$(mktemp)
    status=$(curl -sS -o "$body" -w '%{http_code}' \
                  -X POST "${URL}/v1/models/${encoded}" || true)
    case "$status" in
        200) echo "✓ ${model} (already cached)" ;;
        201) echo "↓ ${model} (downloaded)" ;;
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
echo "STT models installed in Speaches:"
curl -fsS "${URL}/v1/models" | python3 -c '
import json, sys
for m in json.load(sys.stdin)["data"]:
    if m.get("task") == "automatic-speech-recognition":
        print("  -", m["id"])'
