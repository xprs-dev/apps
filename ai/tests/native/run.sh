#!/bin/sh
# Native tests for the AI add-on: main.c against a mock HAL
# (tests/native/hal_mock.c). Not part of the wasm build; run:
#   sh tests/native/run.sh
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/../.."
OUT="${TMPDIR:-/tmp}/ai_native_test"
${CC:-cc} -O0 -g -I"$HERE" -I"$SRC/../hal" -Wno-attributes -Wall -Wextra \
  "$HERE/test_ai.c" "$HERE/hal_mock.c" "$SRC/main.c" -o "$OUT"
exec "$OUT"
