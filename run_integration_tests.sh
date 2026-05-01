#!/usr/bin/env bash
# run_integration_tests.sh — build + run the **integration** test suite.
#
# Integration tests consume real on-disk assets (Silero VAD model,
# future: Whisper models, Piper voices) and may talk to live local
# services (llama-server, whisper-server, piper.http_server). They
# resolve dep paths via the same XDG defaults main.cpp uses, so on a
# fully-provisioned dev workstation no env vars are required.
#
# If a dep is missing the affected test cases skip cleanly — they will
# not fail.
#
# Usage:
#   ./run_integration_tests.sh              # = dev
#   ./run_integration_tests.sh dev
#   ./run_integration_tests.sh debug
#
# Forwards extra args to the doctest binary, e.g.:
#   ./run_integration_tests.sh dev --test-case='SileroVad*'

set -euo pipefail

PRESET="${1:-dev}"
shift || true

case "$PRESET" in
    dev|debug) ;;
    -h|--help)
        sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'
        exit 0
        ;;
    release)
        echo "run_integration_tests.sh: 'release' preset disables tests (ACVA_BUILD_TESTS=OFF); use dev or debug" >&2
        exit 2
        ;;
    *)
        echo "run_integration_tests.sh: unknown preset '$PRESET' (expected dev|debug)" >&2
        exit 2
        ;;
esac

cd "$(dirname "$0")"

./build.sh "$PRESET" --target acva_integration_tests

exec "_build/${PRESET}/tests/acva_integration_tests" "$@"
