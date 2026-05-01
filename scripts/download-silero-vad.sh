#!/usr/bin/env bash
# Download the Silero VAD ONNX model into ${XDG_DATA_HOME}/acva/models/.
#
# The path here matches the default cfg.vad.model_path entry in
# config/default.yaml. After running this script, set:
#
#   vad:
#     model_path: "${HOME}/.local/share/acva/models/silero_vad.onnx"
#
# (or the equivalent absolute path for your $XDG_DATA_HOME).
#
# The model is fetched from the official snakers4/silero-vad GitHub
# repo at a pinned commit/release. Roughly 2 MB on disk.
set -euo pipefail

DEST_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/acva/models"
DEST_FILE="${DEST_DIR}/silero_vad.onnx"
URL="https://github.com/snakers4/silero-vad/raw/v5.1.2/src/silero_vad/data/silero_vad.onnx"

mkdir -p "${DEST_DIR}"

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
