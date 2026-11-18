#!/bin/sh
# Native tests for the Compiler add-on's AI editor: main.c against a mock HAL
# (tests/native/hal_mock.c). Not part of the wasm build; run:
#   sh tests/native/run.sh
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/../.."
OUT="${TMPDIR:-/tmp}/compiler_native_test"
${CC:-cc} -O0 -g -I"$HERE" -I"$SRC/../hal" -Wno-attributes -Wall -Wextra \
  "$HERE/test_compiler.c" "$HERE/hal_mock.c" "$SRC/main.c" -o "$OUT"
exec "$OUT"
