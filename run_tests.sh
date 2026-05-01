#!/usr/bin/env bash
# run_tests.sh — build the test binary for a preset and run the suite.
#
# Usage:
#   ./run_tests.sh              # = dev
#   ./run_tests.sh dev
#   ./run_tests.sh debug
#
# Forwards any extra args to the doctest binary. Examples:
#   ./run_tests.sh dev --test-case='paths*'
#   ./run_tests.sh debug --duration --reporters=console
#
# This script always rebuilds first (cheap when nothing changed) so
# stale binaries can't silently report success.

set -euo pipefail

PRESET="${1:-dev}"
shift || true

case "$PRESET" in
    dev|debug) ;;
    -h|--help)
        sed -n '2,14p' "$0" | sed 's/^# \{0,1\}//'
        exit 0
        ;;
    release)
        echo "run_tests.sh: 'release' preset disables tests (ACVA_BUILD_TESTS=OFF); use dev or debug" >&2
        exit 2
        ;;
    *)
        echo "run_tests.sh: unknown preset '$PRESET' (expected dev|debug)" >&2
        exit 2
        ;;
esac

cd "$(dirname "$0")"

./build.sh "$PRESET" --target acva_tests

exec "_build/${PRESET}/tests/acva_tests" "$@"
