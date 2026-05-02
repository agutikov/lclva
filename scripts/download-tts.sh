#!/usr/bin/env bash
# download-tts.sh — install Speaches' TTS voices.
#
# Speaches owns its own installation: POST /v1/models/{id} triggers
# a HuggingFace download into the container's HF cache (bind-mounted
# under ${XDG_DATA_HOME}/acva/models/speaches/). Idempotent — 200 if
# already cached, 201 on first download.
#
# Sibling type-organized scripts:
#   download-llm.sh — LLM GGUFs for llama-server
#   download-stt.sh — Speaches STT model
#   download-vad.sh — Silero VAD ONNX
#   download-assets.sh — umbrella that runs all four
#
# Voice list mirrors `cfg.tts.voices` in `config/default.yaml`. Add
# rows here when adding a new language; the per-language voice id
# also goes in default.yaml so the orchestrator can route by lang.
#
# Requires Speaches reachable at ${ACVA_SPEACHES_URL:-http://127.0.0.1:8090}.

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

VOICES=$(cat <<'EOF'
# English (en)
speaches-ai/piper-en_US-amy-medium
# Russian (ru_RU): all four upstream voices are pulled so users can
# pick by ear via cfg.tts.voices[ru].voice_id. Default selection lives
# in config/default.yaml — `ruslan` is the M5 default.
speaches-ai/piper-ru_RU-denis-medium
speaches-ai/piper-ru_RU-dmitri-medium
speaches-ai/piper-ru_RU-irina-medium
speaches-ai/piper-ru_RU-ruslan-medium
EOF
)

while IFS= read -r voice; do
    [[ -z "$voice" || "$voice" =~ ^[[:space:]]*# ]] && continue
    encoded=$(printf '%s' "$voice" | sed 's| |%20|g')
    body=$(mktemp)
    status=$(curl -sS -o "$body" -w '%{http_code}' \
                  -X POST "${URL}/v1/models/${encoded}" || true)
    case "$status" in
        200) echo "✓ ${voice} (already cached)" ;;
        201) echo "↓ ${voice} (downloaded)" ;;
        *)
            echo "✗ ${voice}: HTTP ${status}" >&2
            head -c 400 "$body" >&2 || true
            echo >&2
            rm -f "$body"
            exit 1
            ;;
    esac
    rm -f "$body"
done <<< "$VOICES"

echo
echo "TTS voices installed in Speaches:"
curl -fsS "${URL}/v1/models" | python3 -c '
import json, sys
for m in json.load(sys.stdin)["data"]:
    if m.get("task") == "text-to-speech":
        print("  -", m["id"])'
