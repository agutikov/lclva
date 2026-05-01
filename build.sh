#!/usr/bin/env bash
# build.sh — configure + build one of the CMake presets.
#
# Usage:
#   ./build.sh              # = dev
#   ./build.sh dev
#   ./build.sh debug
#   ./build.sh release
#
# Forwards any extra args to `cmake --build`. Useful examples:
#   ./build.sh dev -j8
#   ./build.sh dev --target acva_core
#   ./build.sh dev -- -v
#
# Build trees live under _build/<preset>/. The script also refreshes the
# top-level ./compile_commands.json symlink to point at the chosen tree
# so clangd reflects the active preset.

set -euo pipefail

PRESET="${1:-dev}"
shift || true   # drop $1 if it was provided; rest of "$@" passes through

case "$PRESET" in
    dev|debug|release) ;;
    -h|--help)
        sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'
        exit 0
        ;;
    *)
        echo "build.sh: unknown preset '$PRESET' (expected dev|debug|release)" >&2
        exit 2
        ;;
esac

cd "$(dirname "$0")"

# Configure (idempotent — CMake skips when cache is fresh).
cmake --preset "$PRESET"

# Build.
cmake --build -j8 --preset "$PRESET" "$@"

# Refresh the top-level compile_commands.json symlink for clangd. The
# build always emits one under _build/<preset>/; we make the active
# preset the one editors see.
if [[ -f "_build/${PRESET}/compile_commands.json" ]]; then
    ln -sfn "_build/${PRESET}/compile_commands.json" compile_commands.json
fi
