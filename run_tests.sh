#!/usr/bin/env bash
# run_tests.sh — build + run the **unit** test suite.
#
# Pure unit tests, no external deps (no on-disk model files, no live
# backends). Fast feedback loop. For tests that need real assets or
# services, see ./run_integration_tests.sh.
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
        sed -n '2,18p' "$0" | sed 's/^# \{0,1\}//'
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

./build.sh "$PRESET" --target acva_unit_tests

exec "_build/${PRESET}/tests/acva_unit_tests" "$@"
