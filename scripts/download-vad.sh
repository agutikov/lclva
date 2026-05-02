#!/usr/bin/env bash
# download-vad.sh — fetch the Silero VAD ONNX model.
#
# Sibling type-organized scripts:
#   download-llm.sh — LLM GGUFs for llama-server
#   download-stt.sh — Speaches STT model
#   download-tts.sh — Speaches TTS voices
#   download-assets.sh — umbrella that runs all four
#
# Downloads into ${XDG_DATA_HOME}/acva/models/silero/, matching the
# per-engine subfolder layout (siblings: llama.cpp/, speaches/).
# main.cpp resolves cfg.vad.model_path to the same default.
#
# The model is fetched from the official snakers4/silero-vad GitHub
# repo at a pinned commit/release. Roughly 2 MB on disk.
set -euo pipefail

MODELS_ROOT="${XDG_DATA_HOME:-$HOME/.local/share}/acva/models"
DEST_DIR="${MODELS_ROOT}/silero"
DEST_FILE="${DEST_DIR}/silero_vad.onnx"
URL="https://github.com/snakers4/silero-vad/raw/v5.1.2/src/silero_vad/data/silero_vad.onnx"

mkdir -p "${DEST_DIR}"

# Migration: silero_vad.onnx used to live directly under MODELS_ROOT.
# If a legacy copy is there, move it into silero/.
LEGACY="${MODELS_ROOT}/silero_vad.onnx"
if [[ -f "${LEGACY}" && ! -f "${DEST_FILE}" ]]; then
    echo "↻ migration: moving silero_vad.onnx → silero/"
    mv "${LEGACY}" "${DEST_FILE}"
fi
if [[ -f "${LEGACY}" && -f "${DEST_FILE}" ]]; then
    echo "↻ migration: silero_vad.onnx already in silero/, removing legacy copy"
    rm -f "${LEGACY}"
fi

if [[ -f "${DEST_FILE}" ]]; then
    echo "silero_vad.onnx already present at ${DEST_FILE}"
    exit 0
fi

echo "Downloading Silero VAD model from ${URL}"
if command -v curl >/dev/null 2>&1; then
    curl -fL --output "${DEST_FILE}" "${URL}"
elif command -v wget >/dev/null 2>&1; then
    wget -O "${DEST_FILE}" "${URL}"
else
    echo "error: need curl or wget on PATH" >&2
    exit 1
fi

echo "wrote ${DEST_FILE}"
