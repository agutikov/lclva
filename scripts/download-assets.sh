#!/usr/bin/env bash
# download-assets.sh — fetch every model asset acva needs to run.
#
# Calls each type-organized downloader in turn:
#   download-llm.sh   (LLM GGUF for llama-server)
#   download-stt.sh   (Speaches STT model)
#   download-tts.sh   (Speaches TTS voices)
#   download-vad.sh   (Silero VAD ONNX)
#
# The Speaches scripts (stt/tts) require the container to be running.
# The LLM and VAD scripts are plain curl into the host bind-mount and
# work whether Compose is up or not.
#
# All four are idempotent — re-runs skip files already present at the
# expected size. Total disk footprint of the defaults: ~6.5 GB.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "[1/4] LLM weights"
"${HERE}/download-llm.sh"
echo

echo "[2/4] Silero VAD"
"${HERE}/download-vad.sh"
echo

echo "[3/4] Speaches STT"
"${HERE}/download-stt.sh"
echo

echo "[4/4] Speaches TTS"
"${HERE}/download-tts.sh"
echo

echo "All asset downloaders ran successfully."
